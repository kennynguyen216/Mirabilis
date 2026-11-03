#include "texture_cache.h"
#include <doctest/doctest.h>
#include <string_view>

namespace tc=mirabilis::texture_cache;
namespace {
void set_word(std::vector<std::byte>& bytes,size_t offset,uint32_t value) {
    for(int i=0;i<4;++i)bytes[offset+i]=std::byte((value>>(8*i))&255);
}
std::vector<std::byte> dds_fixture() {
    // 8x4, 4x2, 2x1, 1x1: 32 + 16 + 16 + 16 bytes.
    std::vector<std::byte> bytes(148+80);
    for(auto [offset,value]:std::initializer_list<std::pair<size_t,uint32_t>>{
        {0,0x20534444},{4,124},{12,4},{16,8},{28,4},{76,32},{80,4},{84,0x30315844},
        {128,99},{132,3},{140,1}}) set_word(bytes,offset,value);
    return bytes;
}
}

TEST_CASE("BC5 and BC7 DDS validate mip blocks, dimensions and supported resource layouts") {
    const auto source=dds_fixture();const auto parsed=tc::parse_dds(source);
    REQUIRE(parsed);CHECK(parsed->srgb);CHECK(parsed->width==8);CHECK(parsed->height==4);
    REQUIRE(parsed->mips.size()==4);CHECK(parsed->payload.size()==80);
    CHECK(parsed->mips[0].size==32);CHECK(parsed->mips[3].offset==64);CHECK(parsed->mips[3].width==1);
    for(auto [offset,value]:std::initializer_list<std::pair<size_t,uint32_t>>{
        {0,0},{4,120},{12,0},{16,9000},{28,2},{28,32},{76,0},{80,0},{84,0},
        {128,71},{132,4},{136,4},{140,2},{112,0x200000}}) {
        auto invalid=source;set_word(invalid,offset,value);CHECK_FALSE(tc::parse_dds(invalid));
    }
    for(size_t size:{0u,4u,147u,148u,227u,229u}) {
        auto invalid=source;invalid.resize(size);CHECK_FALSE(tc::parse_dds(invalid));
    }
    auto linear=source;set_word(linear,128,98);REQUIRE(tc::parse_dds(linear));CHECK_FALSE(tc::parse_dds(linear)->srgb);
    set_word(linear,128,83);REQUIRE(tc::parse_dds(linear));CHECK(tc::parse_dds(linear)->dxgiFormat==83);
}

TEST_CASE("texture cache rejects stale sources, corrupt payloads and wrong color spaces") {
    constexpr std::string_view text="123456789";
    const auto source=std::as_bytes(std::span(text.data(),text.size()));
    CHECK(tc::crc32(source)==0xcbf43926u);
    const auto dds=dds_fixture();std::vector<std::byte> cache(24);
    constexpr std::string_view magic="MBTCv001";
    for(size_t i=0;i<magic.size();++i)cache[i]=std::byte(magic[i]);
    set_word(cache,8,uint32_t(source.size()));set_word(cache,16,0xcbf43926u);set_word(cache,20,tc::crc32(dds));
    cache.insert(cache.end(),dds.begin(),dds.end());
    REQUIRE(tc::parse_cache(cache,source,true));CHECK_FALSE(tc::parse_cache(cache,source,false));
    for(size_t offset:std::array<size_t,7>{0,8,12,16,20,24,cache.size()-1}) {
        auto invalid=cache;invalid[offset]^=std::byte(1);CHECK_FALSE(tc::parse_cache(invalid,source,true));
    }
    auto changed=std::vector<std::byte>(source.begin(),source.end());changed[0]=std::byte('x');
    CHECK_FALSE(tc::parse_cache(cache,changed,true));
    CHECK_FALSE(tc::parse_cache(std::span(cache).first(171),source,true));
    auto normalDds=dds;set_word(normalDds,128,83);
    std::copy(normalDds.begin(),normalDds.end(),cache.begin()+24);
    set_word(cache,20,tc::crc32(normalDds));
    CHECK_FALSE(tc::parse_cache(cache,source,false));
    CHECK_FALSE(tc::parse_cache(cache,source,true,true));
    REQUIRE(tc::parse_cache(cache,source,false,true));
    CHECK(tc::parse_cache(cache,source,false,true)->dxgiFormat==83);
}
