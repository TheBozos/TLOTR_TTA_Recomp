"""Player-facing migration and timing regression checks; no game data required."""
import configparser
import sys
import tempfile
import io
import subprocess
import tarfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import make_release
import make_build_kit
import tta_build
from unittest.mock import patch
from types import SimpleNamespace

REQUIRED = (
    'TTA Launcher.exe', 'README.txt', 'CHANGELOG.md', 'LICENSES/TMB_Recomp/LICENSE',
    'LICENSES/BozoEngine/LICENSE', 'LICENSES/TLOTR_TTA_Recomp/LICENSE', 'source/VERSION', 'source/release.ini', 'kit/kit_manifest.json',
    'kit/python/python.exe', 'source/tools/tta_build.py', 'source/BozoEngine/CMakeLists.txt',
    'kit/deps/sdl3-src/CMakeLists.txt', 'kit/deps/imgui-src/imgui.cpp',
)


class ReleaseTests(unittest.TestCase):
    def test_retry_drops_failed_feature_checks_after_toolchain_relocation_changes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            cache = root / 'CMakeCache.txt'
            cache.write_text('CMAKE_HOME_DIRECTORY:INTERNAL=' + tta_build.ROOT +
                             '\nBOZO_ENGINE_DIR:PATH=engine\nHAVE_XINPUT_H:INTERNAL=\n', encoding='utf-8')
            (root / 'CMakeFiles').mkdir()
            (root / 'CMakeFiles/old-check').write_bytes(b'failed check')
            stamp = root / '.tta-toolchain-key'
            stamp.write_text('before relocation fix', encoding='utf-8')
            build = tta_build.Build.__new__(tta_build.Build)
            build.cmake_build = folder
            build.disc_dir = folder
            build.game = folder
            build.args = SimpleNamespace(build_type='Release', debug_info='no')
            build.ffmpeg_root = folder
            build.toolchain_key = lambda: 'relocated compiler'
            build.tool = lambda name: name
            build.elf = lambda: str(root / 'own-elf')
            build.fetch_args = lambda: []
            def configure(_command):
                self.assertFalse(cache.exists())
                self.assertFalse((root / 'CMakeFiles').exists())
            build.run = configure
            build.step_configure()
            self.assertEqual(stamp.read_text(encoding='utf-8').strip(), 'relocated compiler')


    def test_source_export_drains_archive_padding_before_waiting(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            archive = base / 'source.tar'
            content = b'native source archive\n'
            with tarfile.open(archive, 'w') as tar:
                entry = tarfile.TarInfo('source.txt')
                entry.size = len(content)
                tar.addfile(entry, io.BytesIO(content))
            producer = base / 'producer.py'
            producer.write_text('import sys\nfrom pathlib import Path\n'
                                'sys.stdout.buffer.write(Path(sys.argv[1]).read_bytes())\n'
                                'sys.stdout.buffer.write(bytes(1048576))\n', encoding='utf-8')
            command = [sys.executable, str(producer), str(archive)]
            code = ('import sys; sys.path.insert(0, %r); import make_release; '
                    'make_release.extract_tar_stream(%r, %r, %r)' %
                    (str(ROOT / 'tools'), command, str(base / 'out'), folder))
            subprocess.run([sys.executable, '-c', code], check=True, timeout=15)
            self.assertEqual((base / 'out/source.txt').read_bytes(), content)


    def test_source_cleanup_cannot_escape_the_installation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'source'
            root.mkdir()
            outside = Path(folder) / 'outside'
            outside.mkdir()
            (outside / 'keep.txt').write_bytes(b'outside source')
            (root / 'release_files.txt').write_text('../outside/keep.txt\n', encoding='utf-8')
            with patch.object(tta_build, 'ROOT', str(root)), self.assertRaises(tta_build.BuildError):
                tta_build.remove_stale_sources()
            self.assertEqual((outside / 'keep.txt').read_bytes(), b'outside source')


    def test_update_removes_the_retired_source_component_and_keeps_player_data(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'source'
            root.mkdir()
            (root / 'release_files.txt').write_text('tools/kept.py\n', encoding='utf-8')
            for name in ('tools/kept.py', 'tools/removed.py', 'PS2Recomp/runtime.cpp', 'PS2Recomp/__pycache__/old.pyc',
                         'custom/untouched.txt', 'game/save.dat'):
                file = root / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(b'preserve or retire by ownership')
            with patch.object(tta_build, 'ROOT', str(root)):
                tta_build.remove_stale_sources()
            self.assertFalse((root / 'PS2Recomp').exists())
            self.assertFalse((root / 'tools/removed.py').exists())
            for name in ('tools/kept.py', 'custom/untouched.txt', 'game/save.dat'):
                self.assertEqual((root/name).read_bytes(), b'preserve or retire by ownership')


    def test_update_never_follows_a_source_directory_link(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'source'
            external = Path(folder) / 'external'
            root.mkdir()
            external.mkdir()
            (root / 'tools').mkdir()
            (root / 'release_files.txt').write_text('tools/kept.py\n', encoding='utf-8')
            (external / 'keep.txt').write_bytes(b'outside source')
            if sys.platform == 'win32':
                import _winapi
                _winapi.CreateJunction(str(external), str(root / 'PS2Recomp'))
                _winapi.CreateJunction(str(external), str(root / 'tools/linked'))
            else:
                (root / 'PS2Recomp').symlink_to(external, target_is_directory=True)
                (root / 'tools/linked').symlink_to(external, target_is_directory=True)
            with patch.object(tta_build, 'ROOT', str(root)):
                tta_build.remove_stale_sources()
            self.assertEqual((external / 'keep.txt').read_bytes(), b'outside source')
            self.assertFalse((root / 'PS2Recomp').exists())
            self.assertTrue((root / 'tools/linked').exists())


    def test_only_native_engine_is_a_submodule(self):
        config = configparser.ConfigParser()
        config.read(ROOT / '.gitmodules')
        self.assertEqual(config.sections(), ['submodule "BozoEngine"'])
        self.assertEqual(config[config.sections()[0]]['path'], 'BozoEngine')


    def test_release_rejects_game_executables(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'bad.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                for name in REQUIRED:
                    archive.writestr('TLOTR_TTA_Recomp_Bozo/' + name, b'fixture')
                archive.writestr('TLOTR_TTA_Recomp_Bozo/source/hidden.dat', b'\x7fELF' + b'\0' * 100)
            self.assertTrue(any('an ELF file' in error for error in make_release.verify_zip(path)))



class DiscBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tta_build.load_engine()

    def test_a_modified_executable_is_rejected(self):
        import hashlib
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in tta_build.DISC_HASHES:
                (root / name).write_bytes(name.encode())
            hashes = {n: hashlib.sha1((root/n).read_bytes()).hexdigest() for n in tta_build.DISC_HASHES}
            b = tta_build.Build.__new__(tta_build.Build)
            b.args = SimpleNamespace(disc=folder)
            with patch.object(tta_build, 'DISC_HASHES', hashes):
                b.step_verify()
                (root/tta_build.ELF_NAME).write_bytes(b'modified')
                with self.assertRaisesRegex(tta_build.BuildError, tta_build.ELF_NAME): b.step_verify()

    def test_only_required_disc_inputs_are_extracted(self):
        for name in tta_build.DISC_FILES: self.assertTrue(tta_build.Build.wanted(name.lower()))
        for name in ('DATA/GAME/GLOBSCEN.SCP', 'DATA/GAME/E01C/E01C01.SCP', 'data/game/e03c/e03c03.scp'):
            self.assertTrue(tta_build.Build.wanted(name), name)
        for name in ('save.dat', 'DATA/GAME/GLOBSCEN.SAS', 'DATA/MOVIES/LOGO/LOGO.PS2', '../SLUS_210.27',
                     'MODULES/UNUSED.IRX', 'DATA/GAME/E01C/X/DEEP.SCP'):
            self.assertFalse(tta_build.Build.wanted(name), name)

    def test_same_sized_old_image_is_replaced(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            selected = root / 'selected.iso'; selected.write_bytes(b'new image')
            game = root / 'game'; game.mkdir()
            (game/'disc.iso').write_bytes(b'old image')
            disc = SimpleNamespace(image=SimpleNamespace(size=2048, sector_size=2048), close=lambda: None)
            b = tta_build.Build.__new__(tta_build.Build); b.game = str(game)
            with patch.object(tta_build.ps2_disc, 'Disc', return_value=disc): b.place_image(str(selected))
            self.assertEqual((game/'disc.iso').read_bytes(), b'new image')

    def test_archive_requires_tmb_attribution_and_version(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)/'release.zip'
            with zipfile.ZipFile(p,'w') as z:
                for name in REQUIRED: z.writestr('TLOTR_TTA_Recomp_Bozo/'+name,b'fixture')
            self.assertEqual(make_release.verify_zip(p), [])
            with zipfile.ZipFile(p,'a') as z: z.writestr('TLOTR_TTA_Recomp_Bozo/source/E01C01.SCP',b'game')
            self.assertTrue(any('E01C01.SCP' in e for e in make_release.verify_zip(p)))

class ResumeTests(unittest.TestCase):
    def test_deleted_game_assets_force_staging_again(self):
        with tempfile.TemporaryDirectory() as folder:
            b = tta_build.Build.__new__(tta_build.Build)
            b.game = b.cmake_build = b.disc_dir = folder
            self.assertFalse(b.outputs_present('stage'))
            for name in ('tta'+tta_build.EXE, tta_build.ELF_NAME, 'tta_symbols.txt', 'textures/index.txt', 'meshes/index.txt'):
                p = Path(folder)/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_bytes(b'fixture')
            self.assertTrue(b.outputs_present('stage'))
            (Path(folder)/'meshes/index.txt').unlink()
            self.assertFalse(b.outputs_present('stage'))

class CleanEnvironmentTests(unittest.TestCase):
    def test_windows_system_variables_survive_uppercase_keys(self):
        import os
        values = {'SYSTEMROOT':r'C:\Windows','WINDIR':r'C:\Windows','COMSPEC':r'C:\Windows\System32\cmd.exe',
                  'PATH':r'C:\msys64\ucrt64\bin','MSYSTEM':'UCRT64'}
        for clean in (make_release.clean_env, make_build_kit.clean_env):
            with patch.dict(os.environ,values,clear=True): env = clean()
            for name in ('SYSTEMROOT','WINDIR','COMSPEC'): self.assertEqual(env[name],values[name])
            self.assertNotIn('MSYSTEM',env)
            self.assertNotIn('msys64',env['PATH'])

if __name__ == '__main__': unittest.main()
