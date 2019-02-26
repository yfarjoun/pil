#ifndef ENCODER_H_
#define ENCODER_H_

#include <cassert>
#include <cmath>
#include <unordered_map>

#include "pil.h"
#include "transformer.h"
#include "buffer.h"

namespace pil {

// A: 65, a: 97 -> 0
// C: 67, c: 99 -> 1
// G: 71, g: 103 -> 2
// T: 84, t: 116 -> 3
// N: 78, n: 110 -> 4
static const uint8_t BaseBitTable[256] =
{
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// base encoder
class Encoder : public Transformer {
public:
    Encoder(){}
    Encoder(std::shared_ptr<ResizableBuffer> data) : Transformer(data){}

    inline std::shared_ptr<ResizableBuffer> data() const { return(buffer); }
};

class DictionaryEncoder : public Encoder {
public:
    template <class  T>
    int Encode(std::shared_ptr<ColumnSet> cset,
               std::shared_ptr<ColumnStore> cstore)
    {
        uint32_t tgt_col = cset->columns.size();
        cset->columns.push_back(std::make_shared<ColumnStore>(pil::default_memory_pool()));
        ++cset->n;

        int ret = Encode<T>(cstore, cset->columns[tgt_col]);
        // todo: add transformations arg
        return ret;
    }

    template <class T>
    int Encode(std::shared_ptr<ColumnStore> src,
               std::shared_ptr<ColumnStore> dst)
    {
        //std::cerr << "dict input length=" << src->buffer.length() << std::endl;
        assert(src->n * sizeof(T) == src->buffer.length());
        if(buffer.get() == nullptr) {
            assert(AllocateResizableBuffer(pool_, src->buffer.length() + 16384, &buffer) == 1);
        }

        if(buffer->capacity() < src->buffer.length() + 16384){
           //std::cerr << "here in limit=" << n*sizeof(T) << "/" << buffer->capacity() << std::endl;
           assert(buffer->Reserve(src->buffer.length() + 16384) == 1);
        }

        typedef std::unordered_map<T, uint32_t> map_type;
        map_type map;
        std::vector<T> list;
        T* in = reinterpret_cast<T*>(src->mutable_data());
        T* dat = reinterpret_cast<T*>(buffer->mutable_data());

        for(uint32_t i = 0; i < src->n; ++i) {
            if(map.find(in[i]) == map.end()) {
                map[in[i]] = list.size();
                dat[i] = list.size();
                list.push_back(in[i]);
            } else dat[i] = map.find(in[i])->second;
        }

        std::shared_ptr< ColumnStoreBuilder<T> > b = std::static_pointer_cast< ColumnStoreBuilder<T> >(dst);
        for(size_t i = 0; i < list.size(); ++i) {
            b->Append(list[i]);
        }

        //std::cerr << "b buffer=" << b->buffer.length() << " and src=" << src->buffer.length() << std::endl;
        //memcpy(dst->mutable_data(), dat, b->buffer.length());
        b->compressed_size = b->buffer.length();
        b->buffer.UnsafeSetLength(b->buffer.length());
        b->transformation_args.push_back(std::make_shared<TransformMeta>(PIL_ENCODE_DICT, b->buffer.length(), b->buffer.length()));

        // todo: test: replacing original data with dict-encoded data
        for(uint32_t i = 0; i < src->n; ++i) {
            dat[i] = map[dat[i]];
        }

        //std::cerr << "map=" << list.size() << " out of " << src->n << std::endl;
        //std::cerr << "dst=" << dst->n << " sz=" << dst->uncompressed_size << std::endl;
        return(1);
    }
};

class BaseBitEncoder : public Encoder {
public:
    int Encode(const uint8_t* in, const uint32_t n_in) {
        int n_bytes = std::ceil((float)n_in / 4); // 2 bits per symbol

        if(buffer.get() == nullptr) {
            assert(AllocateResizableBuffer(pool_, n_bytes + 64, &buffer) == 1);
        }

        if(buffer->capacity() < n_in + 64) {
            assert(buffer->Reserve(n_in + 64) == 1);
        }

        memset(buffer->mutable_data(), 0, buffer->capacity()); // zero out buffer
        uint8_t* out = buffer->mutable_data();

        uint32_t offset = 0; // offset into in byte stream
        assert(n_bytes > 1);
        for(int i = 0; i < n_bytes - 1; ++i) {
            out[i] = 0;
            //std::cerr << "b=" << std::bitset<8>(out[i]) << std::endl;
            for(int j = 0; j < 4; ++j, ++offset) {
                //std::cerr << "in=" << in[offset] << " -> " << (int)BaseBitTable[in[offset]] << " @ " << 2*j << std::endl;
                out[i] |= (BaseBitTable[in[offset]] << 2*j);
            }
            //std::cerr << "a=" << std::bitset<8>(out[i]) << std::endl;
        }

        uint32_t residual = n_in % 4;
        //std::cerr << "residual=" << residual << std::endl;
        for(uint32_t j = 0; j < residual; ++j, ++offset) {
            out[n_bytes - 1] |= BaseBitTable[in[offset]] << j;
        }

        //std::cerr << "new packed=" << n_in << "->" << n_bytes << std::endl;

        return(n_bytes);
    }

};

}



#endif /* ENCODER_H_ */
