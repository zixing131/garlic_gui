#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <cstdlib>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.value(1).contains("slow")) QThread::sleep(30);
    if (args.value(1).contains("crash")) std::abort();
    const int index = args.indexOf("-I");
    if (index > 0) {
        QFile file(args.value(index + 1));
        if (!file.open(QIODevice::WriteOnly)) return 2;
        if (args.value(1).contains("large-index")) {
            const QByteArray padding(1024 * 1024, ' ');
            for (int i = 0; i < 257; ++i) {
                file.write("{\"name\":\"demo/Class" + QByteArray::number(i) + "\"}");
                file.write(padding);
                file.write("\n");
            }
        } else if (args.value(1).contains("truncated-index")) {
            file.write("{\"name\":\"demo/Valid\"}\n{\"name\":");
        } else file.write("{\"name\":\"../outside\"}\n");
    }
    return 0;
}
