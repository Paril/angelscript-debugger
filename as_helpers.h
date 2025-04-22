// MIT Licensed
// see https://github.com/Paril/angelscript-debugger

#pragma once

#include <angelscript.h>
#include <string_view>
#include <variant>
#include <vector>

#ifdef __cpp_lib_format
#include <format>
namespace fmt = std;
#else
#include <fmt/format.h>
#endif

// simple std::expected-like
template<typename T>
struct asIDBExpected
{
private:
    std::variant<std::string_view, T> data;

public:
    constexpr asIDBExpected() :
        data("unknown error")
    {
    }

    constexpr asIDBExpected(const std::string_view v) :
        data(std::in_place_index<0>, v)
    {
    }

    constexpr asIDBExpected(T &&v) :
        data(std::in_place_index<1>, std::move(v))
    {
    }

    constexpr asIDBExpected(const T &v) :
        data(std::in_place_index<1>, v)
    {
    }

    constexpr asIDBExpected(asIDBExpected<void> &&v);

    asIDBExpected(const asIDBExpected<T> &) = default;
    asIDBExpected(asIDBExpected<T> &&) = default;
    asIDBExpected &operator=(const asIDBExpected<T> &) = default;
    asIDBExpected &operator=(asIDBExpected<T> &&) = default;

    constexpr asIDBExpected &operator=(const T &v)
    {
        data = v;
        return *this;
    }

    constexpr asIDBExpected &operator=(T &&v)
    {
        data = std::move(v);
        return *this;
    }

    constexpr bool has_value() const
    {
        return data.index() == 1;
    }
    constexpr explicit operator bool() const
    {
        return has_value();
    }

    constexpr const std::string_view &error() const
    {
        return std::get<0>(data);
    }
    constexpr const T &value() const
    {
        return std::get<1>(data);
    }
    constexpr T &value()
    {
        return std::get<1>(data);
    }
};

template<>
struct asIDBExpected<void>
{
private:
    std::string_view err;

public:
    constexpr asIDBExpected() :
        err("unknown error")
    {
    }

    constexpr asIDBExpected(const std::string_view v) :
        err(v)
    {
    }

    constexpr const std::string_view &error() const
    {
        return err;
    }
};

template<typename T>
constexpr asIDBExpected<T>::asIDBExpected(asIDBExpected<void> &&v) :
    data(std::in_place_index<0>, v.error())
{
}

template<class E>
asIDBExpected(E) -> asIDBExpected<void>;

// helper class that is similar to an any,
// storing a value of any type returned by AS
// and managing the ref count.
struct asIDBValue
{
public:
    asIScriptEngine *engine = nullptr;
    int              typeId = 0;
    asITypeInfo     *type = nullptr;

    union {
        asBYTE  u8;
        asWORD  u16;
        asDWORD u32;
        asQWORD u64;
        float   flt;
        double  dbl;
        void   *obj;
    } value {};

    asIDBValue() = default;
    asIDBValue(asIScriptEngine *engine, void *ptr, int typeId, bool reference = false);
    asIDBValue(const asIDBValue &other);
    asIDBValue(asIDBValue &&other) noexcept;

    asIDBValue &operator=(const asIDBValue &other);
    asIDBValue &operator=(asIDBValue &&other) noexcept;

    ~asIDBValue();
    void Release();

    bool IsValid() const
    {
        return typeId != 0;
    }

    template<typename T>
    T *GetPointer(bool as_reference = false) const
    {
        if (typeId == 0)
            return nullptr;
        else if (typeId & asTYPEID_MASK_OBJECT)
        {
            if ((typeId & asTYPEID_OBJHANDLE) && as_reference)
                return reinterpret_cast<T *>(const_cast<void **>(&value.obj));
            return reinterpret_cast<T *>(value.obj);
        }
        return reinterpret_cast<T *>(const_cast<asQWORD *>(&value.u64));
    }

    void SetArgument(asIScriptContext *ctx, asUINT index) const;
};

// helper class to deal with foreach iteration.
class asIDBObjectIteratorHelper
{
public:
    asITypeInfo                     *type;
    void                            *obj;
    asIScriptFunction               *opForBegin, *opForEnd, *opForNext;
    std::vector<asIScriptFunction *> opForValues;

    asITypeInfo *iteratorType = nullptr;
    int          iteratorTypeId = 0;

    std::string_view error;

    asIDBObjectIteratorHelper(asITypeInfo *type, void *obj);

    constexpr bool IsValid() const
    {
        return opForBegin != nullptr;
    }
    constexpr explicit operator bool() const
    {
        return IsValid();
    }

    // individual access
    asIDBValue Begin(asIScriptContext *ctx) const;
    void       Value(asIScriptContext *ctx, const asIDBValue &val, size_t index) const;
    asIDBValue Next(asIScriptContext *ctx, const asIDBValue &val) const;
    bool       End(asIScriptContext *ctx, const asIDBValue &val) const;

    // O(n) helper for length
    size_t CalculateLength(asIScriptContext *ctx) const;

private:
    bool Validate();
};

/* -*- mode: c; c-file-style: "k&r" -*-

  strnatcmp.c -- Perform 'natural order' comparisons of strings in C.
  Copyright (C) 2000, 2004 by Martin Pool <mbp sourcefrog net>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
template<bool case_sensitive = true>
struct asIDBNatCmp
{
    static constexpr bool nat_isdigit(char a)
    {
        return a >= '0' && a <= '9';
    }

    static constexpr bool nat_isspace(char a)
    {
        return a == ' ' || a == '\n' || a == '\r' || a == '\t';
    }

    static constexpr char nat_toupper(char a)
    {
        if (a >= 'a' && a <= 'z')
            return a - ('a' - 'A');
        return a;
    }

    static constexpr int compare_right(const char *a, const char *b)
    {
        int bias = 0;

        /* The longest run of digits wins.  That aside, the greatest
       value wins, but we can't know that it will until we've scanned
       both numbers to know that they have the same magnitude, so we
       remember it in BIAS. */
        for (;; a++, b++)
        {
            if (!nat_isdigit(*a) && !nat_isdigit(*b))
                return bias;
            else if (!nat_isdigit(*a))
                return -1;
            else if (!nat_isdigit(*b))
                return +1;
            else if (*a < *b)
            {
                if (!bias)
                    bias = -1;
            }
            else if (*a > *b)
            {
                if (!bias)
                    bias = +1;
            }
            else if (!*a && !*b)
                return bias;
        }

        return 0;
    }

    static constexpr int compare_left(const char *a, const char *b)
    {
        /* Compare two left-aligned numbers: the first to have a
           different value wins. */
        for (;; a++, b++)
        {
            if (!nat_isdigit(*a) && !nat_isdigit(*b))
                return 0;
            else if (!nat_isdigit(*a))
                return -1;
            else if (!nat_isdigit(*b))
                return +1;
            else if (*a < *b)
                return -1;
            else if (*a > *b)
                return +1;
        }

        return 0;
    }

    constexpr inline int operator()(const char *a, const char *b) const
    {
        int  ai, bi;
        char ca, cb;
        int  fractional, result;

        ai = bi = 0;
        while (1)
        {
            ca = a[ai];
            cb = b[bi];

            /* skip over leading spaces or zeros */
            while (nat_isspace(ca))
                ca = a[++ai];

            while (nat_isspace(cb))
                cb = b[++bi];

            /* process run of digits */
            if (nat_isdigit(ca) && nat_isdigit(cb))
            {
                fractional = (ca == '0' || cb == '0');

                if (fractional)
                {
                    if ((result = compare_left(a + ai, b + bi)) != 0)
                        return result;
                }
                else
                {
                    if ((result = compare_right(a + ai, b + bi)) != 0)
                        return result;
                }
            }

            if (!ca && !cb)
            {
                /* The strings compare the same.  Perhaps the caller
                       will want to call strcmp to break the tie. */
                return 0;
            }

            if constexpr (!case_sensitive)
            {
                ca = nat_toupper(ca);
                cb = nat_toupper(cb);
            }

            if (ca < cb)
                return -1;
            else if (ca > cb)
                return +1;

            ++ai;
            ++bi;
        }
    }

    constexpr inline int operator()(const std::string_view &a, const std::string_view &b) const
    {
        return asIDBNatCmp<case_sensitive>()(a.data(), b.data()) < 0;
    }

    constexpr inline int operator()(const std::string &a, const std::string &b) const
    {
        return asIDBNatCmp<case_sensitive>()(a.c_str(), b.c_str()) < 0;
    }
};

using asIDBNatICmp = asIDBNatCmp<false>;

template<bool case_sensitive = true>
struct asIDBNatLess
{
    constexpr inline bool operator()(const char *a, const char *b) const
    {
        return asIDBNatCmp<case_sensitive>()(a, b) < 0;
    }

    constexpr inline bool operator()(const std::string_view &a, const std::string_view &b) const
    {
        return asIDBNatCmp<case_sensitive>()(a.data(), b.data()) < 0;
    }

    constexpr inline bool operator()(const std::string &a, const std::string &b) const
    {
        return asIDBNatCmp<case_sensitive>()(a.c_str(), b.c_str()) < 0;
    }
};

using asIDBNatILess = asIDBNatLess<false>;