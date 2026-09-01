#include "wavetables.h"

namespace msynth {

void WaveTable::allocate(int frames) {
    frameCount = static_cast<u8>(frames);
    size_t total = 0;
    for (int m = 0; m < MIP_LEVELS; ++m) {
        mip_offset_[m] = total;
        total += static_cast<size_t>(frames) * mip_table_size(m);
        int lg = 0;
        while ((1 << lg) < mip_table_size(m)) ++lg;
        mipLog2Size[m] = static_cast<u8>(lg);
    }
    storage.assign(total, 0);
    for (int m = 0; m < MIP_LEVELS; ++m) mipData[m] = storage.data() + mip_offset_[m];
}

}  // namespace msynth
