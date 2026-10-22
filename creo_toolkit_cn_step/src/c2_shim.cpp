/* ==================================================================
 * c2_shim.cpp - CRT compatibility shims for linking the Creo 2 SDK
 * (built with VS2010) with a modern VS2022 toolchain.
 *
 * The VS2010 static CRT exported these as real symbols; VS2015+ made
 * them inline-in-header or changed the ABI, so objects inside PTC's
 * protk_dll.lib can no longer resolve them.  This file re-provides:
 *
 *   __iob_func                          - legacy stdio FILE* array base
 *   c2_exc_ctor_ref                     - body of the VS2010 std::
 *      exception(char const* const&) ctor; build.bat aliases the
 *      VS2010-mangled name onto it with /alternatename.
 *
 * Compiled ONLY when CN_STEP_OLD_SDK is set (see build.bat).
 * ================================================================== */

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <typeinfo>

extern "C" FILE* __acrt_iob_func(unsigned);

extern "C" FILE* __iob_func(void)
{
    return __acrt_iob_func(0);
}

/* VS2010 ABI: std::exception(char const* const& message).
 * Modern std::exception has no such member ctor, so this is a plain
 * function operating on the raw object; the mangled name from VS2010
 * is redirected here on the linker command line. */
extern "C" void c2_exc_ctor_ref(void* self, char const* const* pmsg)
{
    new (self) std::exception(*pmsg);
}

/* std::locale::facet::_Facet_Register - VS2010 static-STL internal,
 * not exported by msvcp100.dll.  A no-op: facets simply never get
 * registered with the global locale.  The plugin never uses C++
 * iostreams, so this is safe for our purposes. */
extern "C" void c2_facet_register(void* /*facet*/)
{
}

/* std::bad_cast(char const*) ctor - same story as _Facet_Register.
 * bad_cast adds no data over exception, so constructing the exception
 * base subobject in place is layout-correct. */
extern "C" void c2_badcast_ctor(void* self, char const* msg)
{
    new (self) std::exception(msg);
}

/* _wenviron - VS2010 static CRT exposed it as data; in modern headers
 * it is a macro over __p__wenviron(), so #undef it and provide the
 * plain data symbol PTC's objects reference.  Mirror the real value
 * once at DLL load time (the host process CRT is already initialised
 * by then). */
#undef _wenviron

extern "C" {

wchar_t** _wenviron = 0;

wchar_t*** __p__wenviron(void);

} /* extern "C" */

/* Sync disabled for now: static initializers in this DLL run inside the
 * Creo process and any fault there kills the whole load.  PTC's code
 * only reads the environment through its own wrappers, so a null
 * _wenviron is survivable; revisit if env access ever matters. */

/* _filbuf / _flsbuf - VS2010 internal stdio refills/flushes, removed
 * from the modern CRT.  Plain fgetc/fputc are close enough for the
 * buffered paths PTC's wrappers use. */
extern "C" int _filbuf(FILE* f)
{
    return f ? fgetc(f) : EOF;
}

extern "C" int _flsbuf(int ch, FILE* f)
{
    return f ? fputc(ch, f) : EOF;
}

/* type_info::operator!= - inline in modern headers, but VS2010 objects
 * reference it as an extern symbol.  operator== is still exported. */
extern "C" bool c2_tieq_ne(const std::type_info& a, const std::type_info& b)
{
    return a != b;
}
