"""Real C transport plus pinned OEM binary/DT mutation rejection."""
import copy
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
import audit_pogo_i2c as audit
from compose_piano_dtb import read_fdt,write_fdt


class PogoI2cTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native=ROOT/"upstream/Mu-Silicium/Binaries/piano/Bringup/I2C/I2C.efi"
        cls.live=ROOT/"private/captures/2026-10-03-piano/live.dtb"
        cls.resource=ROOT/"upstream/Mu-Silicium/Resources/DTBs/piano.dtb"
        cls.image=cls.native.read_bytes();cls.dt=read_fdt(cls.live.read_bytes())

    def test_fixed_inputs_are_review_only(self):
        result=audit.audit(self.native,self.live,self.resource)
        self.assertEqual(result["native_interface"]["method_rvas"],audit.METHOD_RVAS)
        for field in ("hardware_verified","transport_enabled","transport_backend_implemented"):
            self.assertIs(result[field],False)
        self.assertFalse(result["native_interface"]["complete_callable_abi_verified"])
        self.assertFalse(result["resource_dtb_finds_configuration"])
        supplies=result["captured_board"]["supplies"]
        self.assertEqual(supplies["dvdd-supply"]["dt_constraint_microvolts"],1800000)
        self.assertEqual(supplies["vdd-supply"]["dt_constraint_microvolts"],3300000)
        # The initial DT voltage differs; neither is a measurement of native PMIC state.
        self.assertEqual(supplies["dvdd-supply"]["stock_init_microvolts"],1064000)

    def test_install_and_vtable_drift(self):
        for offset in (audit.GUID_RVA,audit.INTERFACE_RVA,audit.INTERFACE_RVA+8,0x1488,0x1498):
            data=bytearray(self.image);data[offset]^=1
            with self.subTest(offset=hex(offset)),self.assertRaises(ValueError):audit.inspect_image(data)

    def test_pe_bounds_nonexecutable_overlap(self):
        pe=struct.unpack_from("<I",self.image,0x3c)[0]
        optional=pe+24;table=optional+struct.unpack_from("<H",self.image,pe+20)[0]
        for offset,value in ((table+20,len(self.image)),(table+12,0xc000),(table+36,0x40000020)):
            data=bytearray(self.image);struct.pack_into("<I",data,offset,value)
            with self.subTest(offset=offset),self.assertRaises(ValueError):audit.inspect_image(data)
        with self.assertRaises(ValueError):audit.inspect_image(self.image[:100])

    def test_rva_is_not_assumed_to_be_file_offset(self):
        sections=[{"rva":0x1000,"raw_offset":0,"virtual_size":8,"raw_size":8,"flags":0x20000000}]
        self.assertEqual(audit.rva_bytes(b"12345678",sections,0x1002,2,True),b"34")
        with self.assertRaises(ValueError):audit.rva_bytes(b"12345678",sections,0x1007,2,True)
        with self.assertRaises(ValueError):audit.rva_bytes(b"12345678",sections,True,1)

    def test_gpio_clock_provider_supply_mux_mutations(self):
        updates=[(audit.CHILD,"irq_pin",struct.pack(">III",0x27,363,0x2001)),
                 (audit.BUS,"clocks",struct.pack(">II",0x35,0x7d)),
                 (audit.CHILD,"dvdd-supply",struct.pack(">I",0x3f9)),
                 ("/soc","#address-cells",struct.pack(">I",2))]
        for path,key,value in updates:
            dt=copy.deepcopy(self.dt);dt["tree"][path][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):audit.dt_facts(write_fdt(dt))
        dt=copy.deepcopy(self.dt)
        p=dt["phandles"][0x6ac];dt["tree"][p+"/mux"]["function"]=b"gpio\0"
        with self.assertRaises(ValueError):audit.dt_facts(write_fdt(dt))

    def test_pin_drift_rejected_even_if_board_facts_still_match(self):
        with tempfile.TemporaryDirectory(prefix="pogo-pin-test-")as directory:
            files=[pathlib.Path(directory)/name for name in ("i2c.efi","live.dtb","resource.dtb")]
            original=[self.image,self.live.read_bytes(),self.resource.read_bytes()]
            for which in range(3):
                for path,data in zip(files,original):path.write_bytes(data)
                data=bytearray(original[which]);data[-1]^=1;files[which].write_bytes(data)
                with self.subTest(input=which),self.assertRaisesRegex(ValueError,"SHA mismatch"):
                    audit.audit(*files)

    def test_actual_c_default_off_and_gated_backend(self):
        include=ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
        sources=[ROOT/"tests/native/PianoPogoTransportTest.c",ROOT/"uefi/core/PianoPogoTransport.c",ROOT/"uefi/core/PianoPogoReport.c"]
        with tempfile.TemporaryDirectory(prefix="pogo-transport-test-")as directory:
            for enabled in (0,1):
                output=pathlib.Path(directory)/("transport-"+str(enabled))
                subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-g","-fshort-wchar",
                    "-fsanitize=address,undefined","-DPIANO_POGO_RUNTIME_READ_EXPERIMENT="+str(enabled),
                    "-I"+str(include),"-I"+str(include/"X64"),"-I"+str(ROOT/"uefi/core"),
                    *map(str,sources),"-o",str(output)],check=True)
                subprocess.run([str(output)],check=True)


if __name__=="__main__":unittest.main()
