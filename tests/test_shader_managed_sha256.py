"""Compare the production managed digest against the host crypto implementation, never on Android."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DOTNET = shutil.which(os.environ.get('DOTNET', 'dotnet'))


@unittest.skipUnless(DOTNET, 'needs the .NET 10 SDK')
class ManagedSha256Tests(unittest.TestCase):
    def test_fips_vectors_padding_boundaries_and_large_inputs(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            shutil.copy2(ROOT / 'games/totk/tools/totk_xlate/ManagedSha256.cs', folder / 'ManagedSha256.cs')
            (folder / 'digest.csproj').write_text(
                '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
                '<TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>\n')
            (folder / 'Program.cs').write_text('''using System;
using System.Security.Cryptography;
using System.Text;
static class Program
{
    static void Check(byte[] input)
    {
        Span<byte> actual = stackalloc byte[32];
        ManagedSha256.HashData(input, actual);
        if (!actual.SequenceEqual(SHA256.HashData(input))) throw new Exception("digest mismatch, length " + input.Length);
    }
    static void Main()
    {
        Check(Array.Empty<byte>()); Check(Encoding.ASCII.GetBytes("abc"));
        Check(Encoding.ASCII.GetBytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
        byte[] million = new byte[1000000]; Array.Fill(million, (byte)'a'); Check(million);
        for (int length = 0; length <= 256; length++)
        {
            byte[] input = new byte[length];
            for (int i = 0; i < length; i++) input[i] = (byte)(i * 73 + length);
            Check(input);
        }
        foreach (int length in new[] {1024, 65535, 65536, 65537, 16777216})
        {
            byte[] input = new byte[length];
            for (int i = 0; i < length; i++) input[i] = (byte)(i * 29 + (i >> 8));
            Check(input);
        }
        Console.WriteLine("266 managed SHA-256 comparisons passed");
    }
}
''')
            result = subprocess.run([DOTNET, 'run', '--project', str(folder / 'digest.csproj'), '-c', 'Release'],
                                    capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('266 managed SHA-256 comparisons passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
