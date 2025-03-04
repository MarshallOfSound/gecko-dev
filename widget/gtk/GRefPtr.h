/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GRefPtr_h_
#define GRefPtr_h_

// Allows to use RefPtr<T> with various kinds of GObjects

#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include "mozilla/RefPtr.h"

namespace mozilla {

template <typename T>
struct GObjectRefPtrTraits {
  static void AddRef(T* aObject) { g_object_ref(aObject); }
  static void Release(T* aObject) { g_object_unref(aObject); }
};

#define GOBJECT_TRAITS(type_) \
  template <>                 \
  struct RefPtrTraits<type_> : public GObjectRefPtrTraits<type_> {};

GOBJECT_TRAITS(GtkWidget)
GOBJECT_TRAITS(GMenu)
GOBJECT_TRAITS(GMenuItem)
GOBJECT_TRAITS(GSimpleAction)
GOBJECT_TRAITS(GSimpleActionGroup)
GOBJECT_TRAITS(GDBusProxy)
GOBJECT_TRAITS(GdkDragContext)

#undef GOBJECT_TRAITS

template <>
struct RefPtrTraits<GVariant> {
  static void AddRef(GVariant* aVariant) { g_variant_ref(aVariant); }
  static void Release(GVariant* aVariant) { g_variant_unref(aVariant); }
};

}  // namespace mozilla

#endif
