#ifndef OMMESH_BELLE_QT4_ITERATORS_HPP
#define OMMESH_BELLE_QT4_ITERATORS_HPP

// The Symbian SDK pairs Qt 4 with libc++'s inline ABI namespace. Qt's
// qiterator.h makes two legacy forward declarations in the outer std
// namespace, which become ambiguous once a standard header defines the tags.
// Hide those declarations while this one header is initially processed.
#include <iterator>
#include <QtCore/qglobal.h>
#define std ommesh_qt4_legacy_iterator_tags
#include <QtCore/qiterator.h>
#undef std

#endif
