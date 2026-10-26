/* No Vulkan instance/device: exercise the production dependency selector directly. */
#include "vk_shader.c"
#include <assert.h>

int main(void) {
    answers_t a = {0};
    const char *targets[] = {"1d", "2d", "3d", "cube", "2darray", "buffer"};
    for (unsigned i = 0; i < sizeof targets / sizeof *targets; i++) {
        a.has_tex[7] = 1;
        snprintf(a.tex[7], sizeof a.tex[7], "%s", targets[i]);
        assert(dep_ok(&a, "tex", 7, "*"));
        assert(!dep_ok(&a, "tex", 7, "*_depth"));
        assert(dep_ok(&a, "tex", 7, targets[i]));
        snprintf(a.tex[7], sizeof a.tex[7], "%s_depth", targets[i]);
        assert(!dep_ok(&a, "tex", 7, "*"));
        assert(dep_ok(&a, "tex", 7, "*_depth"));
        assert(!dep_ok(&a, "tex", 7, targets[i]));
    }
    assert(dep_ok(&a, "tex", 0, "*")); /* absent binding defaults to non-depth 2D */
    assert(!dep_ok(&a, "tex", 0, "*_depth"));
    assert(!dep_ok(&a, "img", 0, "*"));
    assert(!dep_ok(&a, "attr", 0, "*"));
    assert(!dep_ok(&a, "fout", 0, "*"));
    assert(!dep_ok(&a, "unknown", 0, "*"));
    assert(deps_match("tex:7=*_depth;tex:0=*;attr:0=float", &a));
    assert(!deps_match("tex:7=*;tex:0=*;attr:0=float", &a));
    assert(!deps_match("tex:7=*_depth;attr:0=uint", &a));
    assert(!dep_ok(&a, "tex", 7, "2d_depth")); /* concrete dimensions stay exact */
    puts("shader dependency wildcard matching passed");
    return 0;
}
