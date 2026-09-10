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
        file.write("{\"name\":\"../outside\"}\n");
    }
    return 0;
}
