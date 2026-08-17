// Catch2 prints unknown types as {?}, which turns a failed comparison of two
// playlist orderings into two identical-looking rows of question marks. Teaching
// it the handful of Qt types these tests compare costs a few lines and makes the
// failure readable.

#pragma once

#include <catch2/catch_tostring.hpp>

#include <QString>
#include <QStringList>

#include <string>

namespace Catch {

template <>
struct StringMaker<QString> {
    static std::string convert(const QString& value) {
        return '"' + value.toStdString() + '"';
    }
};

template <>
struct StringMaker<QStringList> {
    static std::string convert(const QStringList& value) {
        std::string text = "[";
        for (qsizetype i = 0; i < value.size(); ++i) {
            if (i > 0) {
                text += ", ";
            }
            text += value.at(i).toStdString();
        }
        return text + ']';
    }
};

}  // namespace Catch
