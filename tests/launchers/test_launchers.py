"""Launcher filesystem/argument contracts; no Gothic installation required."""
import argparse
import configparser
from pathlib import Path
import runpy
import tempfile
import unittest

API = runpy.run_path(str(Path(__file__).resolve().parents[2] / 'scripts/coop-launch.py'))

class Launchers(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='co-op paths with spaces ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.data = self.root / 'Game Data'
        (self.data / 'Data').mkdir(parents=True)
        (self.data / '_work').mkdir()
        self.binary = self.root / 'Local Build/Gothic2Notr.exe'
        self.binary.parent.mkdir(); self.binary.write_bytes(b'local build')

    def args(self, role='host', **changes):
        values = dict(role=role, state_dir=self.root/'Saved Profiles', profile='campaign', data=self.data,
                      engine=self.binary, port=27500, listen='0.0.0.0', address='127.0.0.1:27500',
                      secret_file=None, runtime_bin=None, new=True, load=None, world='coop_test.zen')
        values.update(changes)
        return argparse.Namespace(**values)

    def test_independent_roles_paths_audio_and_persistence(self):
        runtime, command, environment, invitation = API['prepare'](self.args())
        self.assertEqual(command[:3], [str(self.binary), '-g', str(self.data)])
        self.assertFalse(any(x in command for x in ('-test-control', '-test-background', '-devmode')))
        secret = environment['OPENGOTHIC_JOIN_SECRET']
        self.assertNotIn(secret, ' '.join(command))
        ini = configparser.ConfigParser(); ini.read(runtime/'Gothic.ini')
        self.assertEqual(ini['SOUND']['soundEnabled'], '1')
        ini['SOUND']['soundVolume'] = '.3'
        with (runtime/'Gothic.ini').open('w') as stream: ini.write(stream)
        (runtime/'save_slot_0.sav').write_bytes(b'preserved save')
        API['prepare'](self.args())
        ini.read(runtime/'Gothic.ini'); self.assertEqual(ini['SOUND']['soundVolume'], '.3')
        self.assertEqual((runtime/'save_slot_0.sav').read_bytes(), b'preserved save')
        guest, command, guest_environment, _ = API['prepare'](self.args('join', secret_file=invitation))
        self.assertNotEqual(runtime, guest)
        self.assertEqual(guest_environment['OPENGOTHIC_JOIN_SECRET'], secret)
        self.assertEqual(command[-2:], ['-coop-join', '127.0.0.1:27500'])
        self.assertNotIn(secret, (runtime/'launcher.json').read_text())

    def test_checkpoint_import_never_overwrites(self):
        source = self.root/'Original Checkpoint.sav'; source.write_bytes(b'original checkpoint')
        runtime, command, _, _ = API['prepare'](self.args(new=False, load=source))
        self.assertEqual(command[-2:], ['-save', '100'])
        _, command, _, _ = API['prepare'](self.args(new=False, load=source))
        self.assertEqual(command[-2:], ['-save', '101'])
        self.assertEqual(source.read_bytes(), b'original checkpoint')
        self.assertEqual((runtime/'save_slot_100.sav').read_bytes(), b'original checkpoint')

    def test_runtime_lock_and_secret_validation(self):
        runtime, _, _, invitation = API['prepare'](self.args())
        with API['runtime_lock'](runtime):
            with self.assertRaises(ValueError):
                with API['runtime_lock'](runtime): pass
        invitation.write_text('bad invitation')
        with self.assertRaises(ValueError): API['prepare'](self.args())

if __name__ == '__main__': unittest.main()
