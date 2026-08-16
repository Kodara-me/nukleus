#include "ManageModelsWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("Kira Service"));
    application.setApplicationName(QStringLiteral("Kira Service Manager"));
    ManageModelsWindow window;
    window.show();
    return application.exec();
}
