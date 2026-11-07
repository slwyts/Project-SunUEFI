use demosaic::{demosaic, Algorithm, CfaPattern};
use piano_quad::{Converter, Error, Params};

fn patterns() -> [CfaPattern; 4] {
    [
        CfaPattern::quad_bayer_rggb(),
        CfaPattern::quad_bayer_bggr(),
        CfaPattern::quad_bayer_grbg(),
        CfaPattern::quad_bayer_gbrg(),
    ]
}

fn pack(
    width: usize,
    height: usize,
    stride: usize,
    sample: impl Fn(usize, usize) -> u16,
) -> Vec<u8> {
    let mut raw = vec![0xad; stride * height];
    for y in 0..height {
        for x in (0..width).step_by(4) {
            let offset = y * stride + x * 5 / 4;
            raw[offset + 4] = 0;
            for i in 0..4 {
                let value = sample(y, x + i);
                assert!(value < 1024);
                raw[offset + i] = (value >> 2) as u8;
                raw[offset + 4] |= ((value & 3) as u8) << (2 * i);
            }
        }
    }
    raw
}

fn linear() -> Params {
    Params {
        contrast: 0.0,
        gamma: 1.0,
        ..Params::default()
    }
}

#[test]
fn colored_flat_all_orders_crop_phases_and_borders() {
    let (w, h) = (32, 28);
    let colors = [0.8f32, 0.5, 0.3];
    for base in patterns() {
        for dy in 0..4 {
            for dx in 0..4 {
                let cfa = base.shift(dy, dx);
                let raw: Vec<_> = (0..w * h)
                    .map(|i| colors[cfa.color_at(i / w, i % w) as usize])
                    .collect();
                let mut rgb = vec![0.0; 3 * w * h];
                demosaic(&raw, w, h, &cfa, Algorithm::QuadPpg, &mut rgb).unwrap();
                for channel in 0..3 {
                    for value in &rgb[channel * w * h..(channel + 1) * w * h] {
                        assert!(
                            (*value - colors[channel]).abs() < 0.00001,
                            "CFA={cfa} phase={dx},{dy} channel={channel}: {value}"
                        );
                    }
                }
            }
        }
    }
}

#[test]
fn every_measured_sample_survives_without_binning() {
    let (w, h) = (40, 32);
    // Four distinct values within every same-color2x2 block. Averaging it
    // and enlarging would fail the exact measured-channel assertions.
    let raw: Vec<_> = (0..w * h)
        .map(|i| ((i * 73 + 19) % 1024) as f32 / 1023.0)
        .collect();
    for base in patterns() {
        for dy in 0..4 {
            for dx in 0..4 {
                let cfa = base.shift(dy, dx);
                let mut rgb = vec![0.0; 3 * w * h];
                demosaic(&raw, w, h, &cfa, Algorithm::QuadPpg, &mut rgb).unwrap();
                for i in 0..w * h {
                    let channel = cfa.color_at(i / w, i % w) as usize;
                    assert_eq!(rgb[channel * w * h + i], raw[i]);
                }
            }
        }
    }
}

#[test]
fn all_raw10_values_and_padding_are_decoded() {
    let (w, h, stride) = (8, 8, 16);
    let mut converter = Converter::new(w, h, 2).unwrap();
    let params = linear();
    let mut output = vec![0; w * h * 3 / 2];
    for sample in 0..1024 {
        let raw = pack(w, h, stride, |_, _| sample);
        converter.unpack(&raw, stride, &params).unwrap();
        converter.render(&mut output).unwrap();
        let expected = (255.0 * sample as f32 / 1023.0).round() as u8;
        assert!(output[..w * h].iter().all(|&value| value == expected));
        assert!(output[w * h..].iter().all(|&value| value == 128));
    }
    // Amplify the low2bits above a known test pedestal, to make losing them
    // observable even after final8-bit quantization.
    let params = Params {
        black_level: 1000.0,
        ..params
    };
    let mut previous = 0;
    for sample in 1000..1004 {
        converter
            .unpack(&pack(w, h, stride, |_, _| sample), stride, &params)
            .unwrap();
        converter.render(&mut output).unwrap();
        if sample > 1000 {
            assert!(output[0] > previous);
        }
        previous = output[0];
    }
}

#[test]
fn owned_frame_and_controls_survive_dma_requeue() {
    let (w, h, stride) = (16, 12, 32);
    let mut converter = Converter::new(w, h, 0).unwrap();
    let mut params = linear();
    let mut raw = pack(w, h, stride, |_, _| 512);
    let mut output = vec![0; w * h * 3 / 2];
    converter.unpack(&raw, stride, &params).unwrap();
    raw.fill(0); // The capture owner may immediately requeue/overwrite DMA.
    params.digital_gain = 8.0; // Later controls apply only to the next frame.
    converter.render(&mut output).unwrap();
    assert!(output[..w * h].iter().all(|&v| v == 128));
    assert_eq!(converter.render(&mut output), Err(Error::Invalid));
}

#[test]
fn color_controls_and_nv12_range() {
    let (w, h, stride) = (16, 16, 32);
    let cfa = CfaPattern::quad_bayer_bggr();
    let levels = [820, 512, 205];
    let raw = pack(w, h, stride, |y, x| levels[cfa.color_at(y, x) as usize]);
    let mut converter = Converter::new(w, h, 1).unwrap();
    let mut output = vec![0; w * h * 3 / 2];
    converter.unpack(&raw, stride, &linear()).unwrap();
    converter.render(&mut output).unwrap();
    assert!(output[..w * h].iter().all(|&v| (141..=143).contains(&v)));
    assert!(output[w * h..]
        .chunks_exact(2)
        .all(|v| (75..=78).contains(&v[0]) && (171..=174).contains(&v[1])));
    let params = Params {
        saturation: 0.0,
        ..linear()
    };
    converter.unpack(&raw, stride, &params).unwrap();
    converter.render(&mut output).unwrap();
    assert!(output[w * h..].iter().all(|&v| v == 128));
}

#[test]
fn bounds_and_invalid_state_do_not_publish_a_frame() {
    assert!(matches!(Converter::new(7, 8, 0), Err(Error::Invalid)));
    assert!(matches!(Converter::new(8164, 6120, 0), Err(Error::Invalid)));
    assert!(matches!(Converter::new(8, 8, 9), Err(Error::Invalid)));
    let mut converter = Converter::new(8, 8, 0).unwrap();
    let raw = pack(8, 8, 16, |_, _| 1023);
    let mut output = vec![0x55; 96];
    assert_eq!(converter.render(&mut output), Err(Error::Invalid));
    assert_eq!(
        converter.unpack(&raw[..100], 16, &linear()),
        Err(Error::Invalid)
    );
    assert_eq!(converter.unpack(&raw, 9, &linear()), Err(Error::Invalid));
    let bad = Params {
        gamma: f32::NAN,
        ..linear()
    };
    assert_eq!(converter.unpack(&raw, 16, &bad), Err(Error::Invalid));
    assert!(output.iter().all(|&v| v == 0x55));
    converter.unpack(&raw, 16, &linear()).unwrap();
    assert_eq!(converter.render(&mut output[..95]), Err(Error::NoSpace));
    converter.render(&mut output).unwrap();
    assert!(output[..64].iter().all(|&v| v == 255));
}
