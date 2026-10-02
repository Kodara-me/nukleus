#include "../src/manager/ManageModelsWindow.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QtTest>

class ManagerUiTest final : public QObject
{
    Q_OBJECT
private slots:
    void groupsInstalledModelsInTree()
    {
        ManageModelsWindow window;
        auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("installedModelsTree"));
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 1);
        auto *root = tree->topLevelItem(0);
        QCOMPARE(root->text(0), QStringLiteral("Models"));
        QVERIFY(!(root->flags() & Qt::ItemIsSelectable));
        QCOMPARE(root->childCount(), 5);
        const QStringList names = {QStringLiteral("Edition"), QStringLiteral("LLM"),
            QStringLiteral("Image generation"), QStringLiteral("Upscaler"), QStringLiteral("IP-Adapter")};
        for (int index = 0; index < names.size(); ++index) {
            QCOMPARE(root->child(index)->text(0), names[index]);
            QVERIFY(!(root->child(index)->flags() & Qt::ItemIsSelectable));
            QVERIFY(root->child(index)->isExpanded());
        }
    }

    void showsCategoryCompanionFields()
    {
        ManageModelsWindow window;
        window.show();
        auto *category = window.findChild<QComboBox *>();
        auto *companion = window.findChild<QLineEdit *>(QStringLiteral("companionUrl"));
        QVERIFY(category);
        QVERIFY(companion);
        const auto hasLabel = [&window](const QString &text) {
            for (const auto *label : window.findChildren<QLabel *>()) {
                if (label->text() == text && !label->isHidden()) return true;
            }
            return false;
        };
        QVERIFY(hasLabel(QStringLiteral("Optional VAE URL")));
        QVERIFY(!hasLabel(QStringLiteral("SHA-256")));
        for (const QString &name : {QStringLiteral("edition"), QStringLiteral("llm-chat"), QStringLiteral("ip-adapter")}) {
            companion->setText(QStringLiteral("https://example.test/old.gguf"));
            category->setCurrentIndex(category->findData(name));
            QVERIFY(companion->text().isEmpty());
            QVERIFY(!companion->isHidden());
            QVERIFY(companion->isEnabled());
            QVERIFY(hasLabel(name == QStringLiteral("llm-chat") ? QStringLiteral("Optional mmproj URL")
                : name == QStringLiteral("ip-adapter") ? QStringLiteral("Optional companion LoRA URL") : QStringLiteral("Optional VAE URL")));
        }
        category->setCurrentIndex(category->findData(QStringLiteral("upscaler")));
        QVERIFY(companion->isHidden());
        QVERIFY(!companion->isEnabled());
    }
};

QTEST_MAIN(ManagerUiTest)
#include "ManagerUiTest.moc"
