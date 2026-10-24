/* Android's Makefile supplies the build kind; standalone/non-Android compilation stays release by default. */
#pragma once
#ifndef TOTK_DEV_BUILD
#define TOTK_DEV_BUILD 0
#endif
#if TOTK_DEV_BUILD != 0 && TOTK_DEV_BUILD != 1
#error "TOTK_DEV_BUILD must be 0 (release) or 1 (dev)"
#endif
#if TOTK_DEV_BUILD
#define TOTK_BUILD_KIND "dev"
#else
#define TOTK_BUILD_KIND "release"
#endif
