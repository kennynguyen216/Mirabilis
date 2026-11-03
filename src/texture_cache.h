#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

namespace mirabilis::texture_cache {

inline uint32_t crc32(std::span<const std::byte> bytes)
{
    static constexpr auto table=[] {
        std::array<uint32_t,256> values{};
        for (uint32_t i=0;i<256;++i) {
            uint32_t value=i;
            for (int bit=0;bit<8;++bit) value=(value>>1)^((value&1)?0xedb88320u:0u);
            values[i]=value;
        }
        return values;
    }();
    uint32_t value=~0u;
    for (auto byte:bytes) value=(value>>8)^table[(value^std::to_integer<uint8_t>(byte))&255u];
    return ~value;
}

inline std::vector<std::byte> read_bytes(const std::filesystem::path& path,size_t limit)
{
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    const auto size=stream.tellg();
    if (!stream || size<=0 || uint64_t(size)>limit) return {};
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    stream.seekg(0);stream.read(reinterpret_cast<char*>(bytes.data()),size);
    return stream?std::move(bytes):std::vector<std::byte>{};
}

struct BlockMip { uint32_t width,height;size_t offset,size; };
struct BlockImage {
    uint32_t width{},height{},dxgiFormat{};
    bool srgb{};
    std::vector<BlockMip> mips;
    std::vector<std::byte> payload;
};

inline uint32_t word(std::span<const std::byte> bytes,size_t offset)
{
    uint32_t result=0;
    for (int i=0;i<4;++i) result|=uint32_t(std::to_integer<uint8_t>(bytes[offset+i]))<<(8*i);
    return result;
}

// Only full-mip BC5/BC7 2D textures are supported by the asset compiler and uploader.
inline std::optional<BlockImage> parse_dds(std::span<const std::byte> bytes)
{
    if (bytes.size()<148 || word(bytes,0)!=0x20534444 || word(bytes,4)!=124 ||
        word(bytes,76)!=32 || (word(bytes,80)&4u)==0 || word(bytes,84)!=0x30315844 ||
        word(bytes,132)!=3 || word(bytes,136)!=0 || word(bytes,140)!=1 || word(bytes,112)!=0)
        return {};
    const uint32_t format=word(bytes,128),width=word(bytes,16),height=word(bytes,12);
    if ((format!=83 && format!=98 && format!=99) || !width || !height || width>8192 || height>8192 ||
        uint64_t(width)*height>64*1024*1024) return {};
    const uint32_t levels=std::bit_width(std::max(width,height));
    if (std::max(word(bytes,28),1u)!=levels) return {};
    BlockImage result;result.width=width;result.height=height;result.dxgiFormat=format;result.srgb=format==99;
    size_t offset=0;
    for (uint32_t level=0;level<levels;++level) {
        const uint32_t w=std::max(width>>level,1u),h=std::max(height>>level,1u);
        const size_t size=size_t((w+3)/4)*((h+3)/4)*16;
        if (size>bytes.size()-148 || offset>bytes.size()-148-size) return {};
        result.mips.push_back({w,h,offset,size});offset+=size;
    }
    if (bytes.size()!=148+offset) return {};
    result.payload.assign(bytes.begin()+148,bytes.end());
    return result;
}

inline std::filesystem::path cache_path(std::filesystem::path source,bool srgb)
{
    source+=srgb?".srgb.mbc":".linear.mbc";return source;
}

inline std::optional<BlockImage> parse_cache(std::span<const std::byte> cache,
    std::span<const std::byte> source,bool srgb,bool normalOnly=false)
{
    if (cache.size()<24+148) return {};
    constexpr std::array<char,8> magic{'M','B','T','C','v','0','0','1'};
    for (size_t i=0;i<magic.size();++i)
        if (cache[i]!=std::byte(magic[i])) return {};
    const uint64_t sourceSize=word(cache,8)|(uint64_t(word(cache,12))<<32);
    const auto dds=cache.subspan(24);
    if (sourceSize!=source.size() || word(cache,16)!=crc32(source) || word(cache,20)!=crc32(dds)) return {};
    auto result=parse_dds(dds);
    if (!result || result->srgb!=srgb || (result->dxgiFormat==83 && !normalOnly)) return {};
    return result;
}

inline std::optional<BlockImage> load(const std::filesystem::path& sourcePath,
    std::span<const std::byte> source,bool srgb,bool normalOnly=false)
{
    const auto cache=read_bytes(cache_path(sourcePath,srgb),256ull*1024*1024);
    return parse_cache(cache,source,srgb,normalOnly);
}

} // namespace mirabilis::texture_cache
