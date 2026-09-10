#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

namespace HookCode {
inline QString quote(const QString &s) {
    auto json = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
// Use runtime descriptors, never display aliases, so overloads and inner classes resolve correctly.
inline QString generate(const QString &id, bool xposed) {
    const int sep = id.indexOf(";->"), open = id.indexOf('(', sep), close = id.indexOf(')', open);
    if (!id.startsWith('L') || sep < 1 || open < sep || close < open) return {};
    const auto owner = id.mid(1, sep - 1).replace('/', '.');
    auto name = id.mid(sep + 3, open - sep - 3);
    if (name == "<clinit>") return {}; // Class initializers are not ordinary hookable methods.
    QStringList types, names;
    const QString primitives = "ZBCSIJFD";
    const QStringList primitiveNames{"boolean", "byte", "char", "short", "int", "long", "float", "double"};
    for (int i = open + 1; i < close;) {
        int start = i;
        while (i < close && id[i] == '[') ++i;
        if (i >= close) return {};
        bool array = i != start;
        QString type;
        if (id[i] == 'L') {
            int end = id.indexOf(';', i);
            if (end < 0 || end >= close) return {};
            type = id.mid(i + 1, end - i - 1).replace('/', '.');
            i = end + 1;
        } else {
            int primitive = primitives.indexOf(id[i++]);
            if (primitive < 0) return {};
            type = primitiveNames[primitive];
        }
        if (array) type = id.mid(start, i - start).replace('/', '.');
        types << (xposed && !array && primitiveNames.contains(type) ? type + ".class" : quote(type));
        names << QString("arg%1").arg(names.size());
    }
    if (xposed) {
        if (name != "<init>") types.prepend(quote(name));
        types.prepend("classLoader");
        types.prepend(quote(owner));
        types << "new XC_MethodHook() {\n    @Override\n    protected void beforeHookedMethod(MethodHookParam param) throws Throwable {\n        // Inspect param.args here.\n    }\n    @Override\n    protected void afterHookedMethod(MethodHookParam param) throws Throwable {\n        // Inspect param.getResult() here.\n    }\n}";
        return "// Inside handleLoadPackage: ClassLoader classLoader = lpparam.classLoader;\n"
               "// Imports: de.robv.android.xposed.XposedHelpers, de.robv.android.xposed.XC_MethodHook\n"
               "XposedHelpers." + QString(name == "<init>" ? "findAndHookConstructor" : "findAndHookMethod") +
               "(" + types.join(", ") + ");\n";
    }
    if (name == "<init>") name = "$init";
    return "Java.perform(function () {\n    const Target = Java.use(" + quote(owner) +
           ");\n    const method = Target[" + quote(name) + "].overload(" + types.join(", ") +
           ");\n    method.implementation = function (" + names.join(", ") +
           ") {\n        console.log(" + quote(owner + "." + name) +
           ");\n        return method.call(this" + (names.isEmpty() ? QString() : ", " + names.join(", ")) +
           ");\n    };\n});\n";
}
}
