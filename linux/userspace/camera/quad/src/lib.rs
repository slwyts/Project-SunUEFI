// SPDX-License-Identifier: MIT
//! Full-resolution RAW10 Quad CFA conversion for the existing camera owner.
//! No file, device, worker thread or calibration access is hidden here.

use demosaic::{demosaic, Algorithm, CfaPattern};
use std::{ptr, slice, time::Instant};

#[repr(i32)]
#[derive(Debug, PartialEq)]
pub enum Error {
    Invalid = 22,
    NoMemory = 12,
    NoSpace = 28,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Params {
    /// Sensor units before normalization; no guessed factory black level.
    pub black_level: f32,
    pub digital_gain: f32,
    pub white_balance: [f32; 3],
    /// Camera-linear RGB to display-linear RGB, row-major.
    pub ccm: [f32; 9],
    pub saturation: f32,
    /// Fraction of smoothstep mixed into the gamma curve, 0..1.
    pub contrast: f32,
    /// Encoding exponent is 1/gamma.
    pub gamma: f32,
}

impl Default for Params {
    fn default() -> Self {
        Self {
            black_level: 0.0,
            digital_gain: 1.0,
            white_balance: [1.0; 3],
            ccm: [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
            saturation: 1.0,
            contrast: 96.0 / 256.0,
            gamma: 2.2,
        }
    }
}

impl Params {
    fn valid(&self) -> bool {
        self.black_level.is_finite()
            && (0.0..1023.0).contains(&self.black_level)
            && self.digital_gain.is_finite()
            && self.digital_gain > 0.0
            && self.white_balance.iter().all(|v| v.is_finite() && *v > 0.0)
            && self.ccm.iter().all(|v| v.is_finite())
            && self.saturation.is_finite()
            && self.saturation >= 0.0
            && self.contrast.is_finite()
            && (0.0..=1.0).contains(&self.contrast)
            && self.gamma.is_finite()
            && self.gamma > 0.0
    }
}

#[repr(C)]
#[derive(Default, Debug, PartialEq)]
pub struct Metrics {
    pub unpack_ns: u64,
    pub demosaic_ns: u64,
    pub color_ns: u64,
    /// Includes the upstream temporary green plane, excludes caller DMA/NV12.
    pub working_bytes: u64,
}

pub struct Converter {
    width: usize,
    height: usize,
    cfa: CfaPattern,
    input: Vec<f32>,
    rgb: Vec<f32>,
    prepared: Option<Params>,
    unpack_ns: u64,
}

fn zeros(count: usize) -> Result<Vec<f32>, Error> {
    let mut values = Vec::new();
    values
        .try_reserve_exact(count)
        .map_err(|_| Error::NoMemory)?;
    values.resize(count, 0.0);
    Ok(values)
}

impl Converter {
    /// CFA numbers follow the C header: RGGB, BGGR, GRBG, GBRG, as 2x2 cells.
    pub fn new(width: usize, height: usize, cfa: u32) -> Result<Self, Error> {
        if width < 8
            || height < 8
            || width > 8160
            || height > 6120
            || width % 4 != 0
            || height % 4 != 0
        {
            return Err(Error::Invalid);
        }
        let cfa = match cfa {
            0 => CfaPattern::quad_bayer_rggb(),
            1 => CfaPattern::quad_bayer_bggr(),
            2 => CfaPattern::quad_bayer_grbg(),
            3 => CfaPattern::quad_bayer_gbrg(),
            _ => return Err(Error::Invalid),
        };
        let count = width * height;
        Ok(Self {
            width,
            height,
            cfa,
            input: zeros(count)?,
            rgb: zeros(3 * count)?,
            prepared: None,
            unpack_ns: 0,
        })
    }

    /// Copy and normalize all samples before the camera owner requeues DMA.
    pub fn unpack(&mut self, raw: &[u8], stride: usize, params: &Params) -> Result<(), Error> {
        self.prepared = None;
        let payload = self.width * 5 / 4;
        let required = stride
            .checked_mul(self.height - 1)
            .and_then(|n| n.checked_add(payload))
            .ok_or(Error::Invalid)?;
        if stride < payload || raw.len() < required || !params.valid() {
            return Err(Error::Invalid);
        }

        let begin = Instant::now();
        for y in 0..self.height {
            for x in (0..self.width).step_by(4) {
                let offset = y * stride + x * 5 / 4;
                let low = raw[offset + 4];
                for i in 0..4 {
                    let sample =
                        (u16::from(raw[offset + i]) << 2) | u16::from((low >> (2 * i)) & 3);
                    let channel = self.cfa.color_at(y, x + i) as usize;
                    self.input[y * self.width + x + i] = ((f32::from(sample) - params.black_level)
                        / (1023.0 - params.black_level)
                        * params.digital_gain
                        * params.white_balance[channel])
                        .clamp(0.0, 1.0);
                }
            }
        }
        self.unpack_ns = begin.elapsed().as_nanos() as u64;
        self.prepared = Some(*params);
        Ok(())
    }

    /// Consume one CPU-owned RAW frame. The original DMA buffer is no longer
    /// referenced; all controls used here are the ones saved during unpack.
    pub fn render(&mut self, output: &mut [u8]) -> Result<Metrics, Error> {
        let count = self.width * self.height;
        if output.len() < count * 3 / 2 {
            return Err(Error::NoSpace);
        }
        let params = self.prepared.take().ok_or(Error::Invalid)?;
        let begin = Instant::now();
        demosaic(
            &self.input,
            self.width,
            self.height,
            &self.cfa,
            Algorithm::QuadPpg,
            &mut self.rgb,
        )
        .map_err(|_| Error::Invalid)?;
        let demosaic_ns = begin.elapsed().as_nanos() as u64;
        let begin = Instant::now();
        encode_nv12(&self.rgb, self.width, self.height, &params, output);
        Ok(Metrics {
            unpack_ns: self.unpack_ns,
            demosaic_ns,
            color_ns: begin.elapsed().as_nanos() as u64,
            working_bytes: (count * 20 + 4097 * 4) as u64,
        })
    }
}

fn byte(value: f32) -> u8 {
    value.round().clamp(0.0, 255.0) as u8
}

fn encode_nv12(rgb: &[f32], width: usize, height: usize, params: &Params, out: &mut [u8]) {
    // A small interpolated LUT avoids three powf calls per 50MP pixel. It is
    // computed from the accepted controls, not a device-specific calibration.
    let mut tone = [0.0f32; 4097];
    for (i, value) in tone.iter_mut().enumerate() {
        let gamma = (i as f32 / 4096.0).powf(1.0 / params.gamma);
        let curve = gamma * gamma * (3.0 - 2.0 * gamma);
        *value = gamma + params.contrast * (curve - gamma);
    }
    let encode = |value: f32| {
        let position = value.clamp(0.0, 1.0) * 4096.0;
        let lo = (position as usize).min(4095);
        tone[lo] + (tone[lo + 1] - tone[lo]) * (position - lo as f32)
    };
    let count = width * height;
    for y in (0..height).step_by(2) {
        for x in (0..width).step_by(2) {
            let mut chroma_u = 0.0;
            let mut chroma_v = 0.0;
            for dy in 0..2 {
                for dx in 0..2 {
                    let index = (y + dy) * width + x + dx;
                    let source = [rgb[index], rgb[count + index], rgb[2 * count + index]];
                    let mut color = [0.0; 3];
                    for row in 0..3 {
                        color[row] =
                            encode((0..3).map(|c| params.ccm[3 * row + c] * source[c]).sum());
                    }
                    let luma = 0.299 * color[0] + 0.587 * color[1] + 0.114 * color[2];
                    for value in &mut color {
                        *value = (luma + params.saturation * (*value - luma)).clamp(0.0, 1.0);
                    }
                    out[index] =
                        byte(255.0 * (0.299 * color[0] + 0.587 * color[1] + 0.114 * color[2]));
                    chroma_u += -0.168736 * color[0] - 0.331264 * color[1] + 0.5 * color[2];
                    chroma_v += 0.5 * color[0] - 0.418688 * color[1] - 0.081312 * color[2];
                }
            }
            let uv = count + y / 2 * width + x;
            // Only chroma uses the standard 4:2:0 average. All four independent
            // luma positions remain; this is not sensor binning or enlargement.
            out[uv] = byte(128.0 + 255.0 * chroma_u / 4.0);
            out[uv + 1] = byte(128.0 + 255.0 * chroma_v / 4.0);
        }
    }
}

// The existing C camera owner serializes this handle. Buffers and Params are
// caller-owned, valid for the call, and must not overlap each other/the handle.
#[no_mangle]
pub unsafe extern "C" fn piano_quad_create(
    width: u32,
    height: u32,
    cfa: u32,
    output: *mut *mut Converter,
) -> i32 {
    if output.is_null() {
        return -(Error::Invalid as i32);
    }
    *output = ptr::null_mut();
    match Converter::new(width as usize, height as usize, cfa) {
        Ok(converter) => {
            *output = Box::into_raw(Box::new(converter));
            0
        }
        Err(error) => -(error as i32),
    }
}

#[no_mangle]
pub unsafe extern "C" fn piano_quad_params_default(params: *mut Params) {
    if !params.is_null() {
        *params = Params::default();
    }
}

#[no_mangle]
pub unsafe extern "C" fn piano_quad_unpack(
    converter: *mut Converter,
    raw: *const u8,
    raw_bytes: usize,
    stride: usize,
    params: *const Params,
) -> i32 {
    if converter.is_null() || raw.is_null() || params.is_null() {
        return -(Error::Invalid as i32);
    }
    match (*converter).unpack(slice::from_raw_parts(raw, raw_bytes), stride, &*params) {
        Ok(()) => 0,
        Err(error) => -(error as i32),
    }
}

#[no_mangle]
pub unsafe extern "C" fn piano_quad_render(
    converter: *mut Converter,
    output: *mut u8,
    output_bytes: usize,
    metrics: *mut Metrics,
) -> i32 {
    if converter.is_null() || output.is_null() {
        return -(Error::Invalid as i32);
    }
    match (*converter).render(slice::from_raw_parts_mut(output, output_bytes)) {
        Ok(result) => {
            if !metrics.is_null() {
                *metrics = result;
            }
            0
        }
        Err(error) => -(error as i32),
    }
}

#[no_mangle]
pub unsafe extern "C" fn piano_quad_destroy(converter: *mut Converter) {
    if !converter.is_null() {
        drop(Box::from_raw(converter));
    }
}
