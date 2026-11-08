"""Checks that the files generated from your ROM are the ones the author's build produced (hashes of the generated C files, line endings ignored).

    python3 tools/check_generated.py

A hash is not the file: nothing of the game is stored here. If a hash differs, the translation is not the one the kit was released with (a different ROM dump,
a changed tool, or the oracle runs did not cover the same code): the build may still work, but the author has not verified that one.
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPECTED = {
    "mod_lo.c": "6ea6d5062cda2bd2978203b350c5a01a02d825f0d776664690ba9ae0547a1215",
    "mod_hi.c": "48e19c7e21832e02685dcc60ea10a906109bd4c2f6fca32ce87fc945389ba32b",
    "mod_b2.c": "4f08d63f0bd85b03587d7f992c7522d6c508072d21071ac23312f10ece6d8510",
    "mod_ram.c": "abce16f322094d9e7a376e25785bceab47b8fe3a2a9b558bee80524632d8f951",
    "gen_info.c": "ffe8f1dff30b1400eca5981e5efafe9eab865b2fcd896e24b7f5df34de71d001",
    "rom_data.c": "5871b70300a08ceaf96ec1190ac6abfe25448ea0c7c7b12cf57562716eedd67c",
}


def main():
    bad = 0
    for name, want in EXPECTED.items():
        path = os.path.join(ROOT, "extracted", "gen", name)
        if not os.path.exists(path):
            print("missing: %s" % name)
            bad += 1
            continue
        got = hashlib.sha256(open(path, "rb").read().replace(b"\r", b"")).hexdigest()
        print("%-12s %s" %(name, "ok" if got == want else "DIFFERENT (expected %s..., got %s...)" % (want[:12], got[:12])))
        bad += got != want
    if bad:
        sys.exit("generated files do not match the released build (see above)")
    print("generated code matches the released build")


if __name__ == "__main__":
    main()
