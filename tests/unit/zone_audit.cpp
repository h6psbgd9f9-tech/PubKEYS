// Compatibility entry point. The former +1-in-every-nibble audit described
// an obsolete zone algorithm. Run the current independent CPU suite instead.
// Build with zone_search.cpp and platform.cpp; no application globals or GPU.
#include "zone_correctness_test.cpp"
