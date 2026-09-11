#pragma once
#include <QTranslator>
#include <QJsonObject>
#include <QList>
#include <QPair>

// UTF-8 JSON catalogs can be added without rebuilding the application.
class Localization : public QTranslator {
  public:
    using QTranslator::QTranslator;
    bool loadLanguage(const QString &code);
    QString translate(const char *context, const char *source,
                      const char *disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override { return messages_.isEmpty(); }
    static QList<QPair<QString, QString>> languages();
    static QStringList directories();
  private:
    QJsonObject messages_;
};
