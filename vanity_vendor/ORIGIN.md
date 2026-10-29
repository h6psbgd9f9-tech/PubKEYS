# CUDA field arithmetic imported into PubKEYS, 2026-09-15

This port takes the corrected math from BitPazl's `GPUMath_corrected.h`, SHA256
`DD9ED4AD16562BFEA1BEA1111CE9990A884E5B195AE36EDB92D6190FA65E6253`.
The historical BitPazl provenance below is retained. PubKEYS imports no GPUHash.h
or BitPazl hashing kernel: it compares public-key X coordinates.

The adapter is in `../cuda_field.cuh`. GPU multiplication, square and inversion
use this math; the CPU arithmetic remains the independent reference. The exact
final carry, canonical output `[0,p)`, zero inverse, aliasing and unchanged input
arrays are preserved. `UMultSpecial` already uses the exact `UMult` with
`2^32+977`, the successful BitPazl reduction improvement. Inversion uses delayed
62-bit GCD and a five-limb temporary; this is not the exact Safegcd code from
libsecp256k1. Full PTX rewrites rejected by the BitPazl experiment are excluded.

For multiple CUDA translation units, vendor functions are included in an
anonymous namespace and MM64/MSK62 constants have static linkage.
`undef_macros.h` keeps vendor macros local to this include.
Independent tests under `../optimization_test/math_opt_20260915_1633` exercise
60,710 vector pairs / 546,390 checks, including boundary values, canaries,
preserved inputs and aliased buffers. CUDA/application tests cover exact public
keys, interval bounds, mirrors and saturated-output replay. The existing
symmetric 1024-key batch, shared inversion for the next center, CPU candidate
confirmation and output bounds remain in place.

## Historical provenance from BitPazl

Source: https://github.com/FixedPaul/VanitySearch-Bitcrack

Snapshot commit: `66e6f9deda607afc5eaf09731725664f0152fc92`.
Downloaded 2026-09-14. GitHub ZIP SHA256:
`4a825843405aba0be0688604632e47c28b55f53df45cc1465b51d8930078ef2c`.

`GPUHash.h` is an unchanged copy of the source GPU/GPUHash.h.
`GPUMath_corrected.h` preserves GPU/GPUMath.h and its original notices, with
the final product carry and canonical reduction restored in both multiplication
overloads and square; negation of zero is canonicalized. The original imported
file's SHA256 and the mathematical proof are recorded in the experiment's
math_review/REVIEW.md and results.json.

The adapted 1024-key computation is in ../gpu_vanity_scan.cuh. It comes from
GPU/GPUEngine.cu comp_keys. BitPazl retains its range validation, exact hash
comparison, completed-key counters and bounded output/replay. Singular batches
and partial tails use the previously checked generic computation. Independent
thread computation has no block barriers. Existing generator tables are reused.

The original file notices specify GPL version 3. The upstream repository's
LICENSE.txt (GNU Affero GPL version 3) and README.md are copied unchanged here;
all source notices are retained. This file records provenance and modifications.

The CUDA arithmetic and hash implementation was checked against independent
Python integer and hashlib oracles. Detailed inputs, executable/source hashes,
logs and results are under filter_test/vanity_port_20260914/math_gpu_test.
