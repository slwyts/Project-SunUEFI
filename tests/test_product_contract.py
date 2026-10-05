import copy
import importlib.util
import json
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('product_contract',ROOT/'tools/product_contract.py')
contract=importlib.util.module_from_spec(spec);spec.loader.exec_module(contract)


class ProductContractTests(unittest.TestCase):
    def setUp(self):self.data=json.loads((ROOT/'config/piano-product.json').read_text())
    def test_no_required_feature_can_be_disabled_or_removed(self):
        contract.validate(self.data)
        for feature in self.data['features']:
            for mode in ('disable','remove'):
                data=copy.deepcopy(self.data)
                if mode=='disable':data['features'][feature]=False
                else:del data['features'][feature]
                with self.subTest(feature=feature,mode=mode),self.assertRaises(ValueError):contract.validate(data)

    def test_single_core_all_uefi_surfaces_and_boot_policy_are_required(self):
        mutations=[('artifact','other.img'),('shared_core',False),('entry_policy_only',False)]
        for key,value in mutations:
            data=copy.deepcopy(self.data);data[key]=value
            with self.assertRaises(ValueError):contract.validate(data)
        for key,value in (('mode','foreground'),('available_in',['SimpleInit']),('start_once',False),('download_limit_target_bytes',67108864)):
            data=copy.deepcopy(self.data);data['fastboot'][key]=value
            with self.assertRaises(ValueError):contract.validate(data)
        data=copy.deepcopy(self.data);data['boot_policy']['diagnostic_reboot_timer']=True
        with self.assertRaises(ValueError):contract.validate(data)

    def test_diagnostic_manifest_cannot_be_renamed_to_product(self):
        diagnostic={'target':'gui','artifact':'PianoUEFI-product.img','features':self.data['features']}
        with self.assertRaises(ValueError):contract.validate_build_manifest(self.data,diagnostic)


if __name__=='__main__':unittest.main()
