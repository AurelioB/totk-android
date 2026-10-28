/* MIT. Resource names/IDs from lsfg-vk d76e12e (MIT). No proprietary bytecode. */
#include "shaders.hpp"
#include "extract/trans.hpp"
#include <pe-parse/parse.h>
#include <array>
#include <map>
#include <memory>
#include <stdexcept>
namespace {
struct Source {
    std::array<std::vector<uint8_t>,48> dxbc, spirv;
    bool performance{};
    bool invalid{};
};
int resource(void* data,const peparse::resource& r) {
    if (r.type!=peparse::RT_RCDATA || r.name<255 || r.name>302) return 0;
    auto& source=*static_cast<Source*>(data);
    if (r.name>=257 && (r.name>=280)!=source.performance) return 0;
    if (!r.buf || r.buf->bufLen<4 || r.buf->bufLen>(16u<<20) || !source.dxbc[r.name-255].empty()) {
        source.invalid=true; return 0;
    }
    source.dxbc[r.name-255].assign(r.buf->buf,r.buf->buf+r.buf->bufLen);
    return 0;
}
const std::map<std::string,unsigned> ids={
    {"mipmaps",255},{"generate",256},
    {"alpha[0]",267},{"alpha[1]",268},{"alpha[2]",269},{"alpha[3]",270},
    {"beta[0]",275},{"beta[1]",276},{"beta[2]",277},{"beta[3]",278},{"beta[4]",279},
    {"gamma[0]",257},{"gamma[1]",259},{"gamma[2]",260},{"gamma[3]",261},{"gamma[4]",262},
    {"delta[0]",257},{"delta[1]",263},{"delta[2]",264},{"delta[3]",265},{"delta[4]",266},
    {"delta[5]",258},{"delta[6]",271},{"delta[7]",272},{"delta[8]",273},{"delta[9]",274}
};
}
std::function<std::vector<uint8_t>(const std::string&)> lsfg_user_shaders(const char *path,bool performance) {
    auto source=std::make_shared<Source>();
    source->performance=performance;
    auto* pe=peparse::ParsePEFromFile(path);
    if (!pe) throw std::runtime_error("cannot parse user's Lossless.dll");
    peparse::IterRsrc(pe,resource,source.get()); peparse::DestructParsedPE(pe);
    if (source->invalid) throw std::runtime_error("ambiguous or invalid Lossless.dll shader resources");
    for (const auto& [name,id]:ids) {
        const auto& bytes=source->dxbc[id-255+(performance && id>=257 ? 23 : 0)];
        if (bytes.size()<4 || bytes[0]!='D' || bytes[1]!='X' || bytes[2]!='B' || bytes[3]!='C')
            throw std::runtime_error("Lossless.dll lacks selected LSFG 3.1 shader resources");
    }
    return [source](const std::string& name) {
        const auto id=ids.at(name);
        const auto at=id-255+(source->performance && id>=257 ? 23 : 0);
        if (source->spirv[at].empty()) source->spirv[at]=Extract::translateShader(std::move(source->dxbc[at]));
        return source->spirv[at];
    };
}
