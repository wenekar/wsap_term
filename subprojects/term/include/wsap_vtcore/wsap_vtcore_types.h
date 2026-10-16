#ifndef WSAP_VTCORE_TYPES_H
#define WSAP_VTCORE_TYPES_H

#ifndef WSAP_VTCORE_API
#if defined(__GNUC__) || defined(__clang__)
#define WSAP_VTCORE_API __attribute__((visibility("default")))
#else
#define WSAP_VTCORE_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct wsap_vtcore_vector2i {
    int x, y;
};

struct wsap_vtcore_span {
    int begin;
    int end;
};

#ifdef __cplusplus
}
#endif

#endif
