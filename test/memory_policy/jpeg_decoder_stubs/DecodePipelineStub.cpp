#include "DecodePipeline.h"

// Host tests have one core, so the converter always decodes inline.
DecodePipeline::~DecodePipeline() = default;
bool DecodePipeline::worthSplitting() { return false; }
bool DecodePipeline::begin(size_t) { return false; }
bool DecodePipeline::run(DecodeFn, void*, ConsumeFn, void*, int&) { return false; }
