"""Catch ABI drift between the Java declarations and the picoXr C implementation.

JNI's short symbol names do not encode parameter types: omitted arguments can
silently read stack/register garbage instead of failing to resolve the method.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DrawableJniContractTest(unittest.TestCase):
    def test_all_drawable_entry_points_match_java(self):
        java = (ROOT / 'app/src/main/java/com/winlator/xserver/Drawable.java').read_text()
        native = (ROOT / 'app/src/main/cpp/asurfacerenderer/drawable.c').read_text()
        java_types = {'short': 'S', 'byte': 'B', 'int': 'I', 'boolean': 'Z', 'ByteBuffer': 'L', 'Bitmap': 'L'}
        c_types = {'jshort': 'S', 'jbyte': 'B', 'jint': 'I', 'int': 'I', 'jboolean': 'Z', 'jobject': 'L'}
        declarations = re.findall(r'private static native void (\w+)\(([^)]*)\);', java)
        self.assertGreaterEqual(len(declarations), 7)
        for name, args in declarations:
            with self.subTest(method=name):
                expected = [java_types[a.strip().split()[0]] for a in args.split(',')]
                definition = re.search(r'Java_com_winlator_xserver_Drawable_' + name + r'\s*\(([^)]*)\)', native)
                self.assertIsNotNone(definition)
                actual = [c_types[a.strip().split()[0]] for a in definition[1].split(',')[2:]]
                self.assertEqual(expected, actual)


if __name__ == '__main__':
    unittest.main()
