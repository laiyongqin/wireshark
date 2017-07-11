/* preferences_dialog.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "preferences_dialog.h"
#include <ui_preferences_dialog.h>

#include "module_preferences_scroll_area.h"

#ifdef HAVE_LIBPCAP
#ifdef _WIN32
#include "caputils/capture-wpcap.h"
#endif /* _WIN32 */
#endif /* HAVE_LIBPCAP */

#include <epan/prefs-int.h>
#include <epan/decode_as.h>
#include <ui/language.h>
#include <ui/preference_utils.h>
#include <ui/simple_dialog.h>
#include <ui/recent.h>
#include <main_window.h>

#include "syntax_line_edit.h"
#include "qt_ui_utils.h"
#include "uat_dialog.h"
#include "wireshark_application.h"

#include <ui/qt/variant_pointer.h>

#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpacerItem>
#include <QTreeWidgetItemIterator>

// XXX Should we move this to ui/preference_utils?
static GHashTable * pref_ptr_to_pref_ = NULL;
pref_t *prefFromPrefPtr(void *pref_ptr)
{
    return (pref_t *)g_hash_table_lookup(pref_ptr_to_pref_, (gpointer) pref_ptr);
}

static void prefInsertPrefPtr(void * pref_ptr, pref_t * pref)
{
    if ( ! pref_ptr_to_pref_ )
        pref_ptr_to_pref_ = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

    gpointer key = (gpointer) pref_ptr;
    gpointer val = (gpointer) pref;

    /* Already existing entries will be ignored */
    if ( (pref = (pref_t *)g_hash_table_lookup(pref_ptr_to_pref_, key) ) == NULL )
        g_hash_table_insert(pref_ptr_to_pref_, key, val);
}

enum {
    module_type_ = 1000,
    advanced_type_
};

enum {
    adv_name_col_,
    adv_status_col_,
    adv_type_col_,
    adv_value_col_
};

enum {
    stacked_role_ = Qt::UserRole + 1,   // pd_ui_->stackedWidget
    module_name_role_,                  // QString
    mpsa_role_                          // QWidget *
};

class AdvancedPrefTreeWidgetItem : public QTreeWidgetItem
{
public:
    AdvancedPrefTreeWidgetItem(pref_t *pref, module_t *module) :
        QTreeWidgetItem (advanced_type_),
        pref_(pref),
        module_(module)
    {}
    pref_t *pref() { return pref_; }
    void updatePref() { emitDataChanged(); }

    virtual QVariant data(int column, int role) const {
        bool is_default = prefs_pref_is_default(pref_);
        switch (role) {
        case Qt::DisplayRole:
            switch (column) {
            case adv_name_col_:
            {
                QString full_name = QString(module_->name ? module_->name : module_->parent->name);
                full_name += QString(".%1").arg(prefs_get_name(pref_));
                return full_name;
            }
            case adv_status_col_:
                if ((prefs_get_type(pref_) == PREF_UAT && (prefs_get_gui_type(pref_) == GUI_ALL || prefs_get_gui_type(pref_) == GUI_QT))|| prefs_get_type(pref_) == PREF_CUSTOM) {
                    return QObject::tr("Unknown");
                } else if (is_default) {
                    return QObject::tr("Default");
                } else {
                    return QObject::tr("Changed");
                }
            case adv_type_col_:
                return QString(prefs_pref_type_name(pref_));
            case adv_value_col_:
            {
                QString cur_value = gchar_free_to_qstring(prefs_pref_to_str(pref_, pref_stashed)).remove(QRegExp("\n\t"));
                return cur_value;
            }
            default:
                break;
            }
            break;
        case Qt::ToolTipRole:
            switch (column) {
            case adv_name_col_:
                return QString("<span>%1</span>").arg(prefs_get_description(pref_));
            case adv_status_col_:
                return QObject::tr("Has this preference been changed?");
            case adv_type_col_:
            {
                QString type_desc = gchar_free_to_qstring(prefs_pref_type_description(pref_));
                return QString("<span>%1</span>").arg(type_desc);
            }
            case adv_value_col_:
            {
                QString default_value = gchar_free_to_qstring(prefs_pref_to_str(pref_, pref_stashed));
                return QString("<span>%1</span>").arg(
                            default_value.isEmpty() ? default_value : QObject::tr("Default value is empty"));
            }
            default:
                break;
            }
            break;
        case Qt::FontRole:
            if (!is_default && treeWidget()) {
                QFont font = treeWidget()->font();
                font.setBold(true);
                return font;
            }
            break;
        default:
            break;
        }
        return QTreeWidgetItem::data(column, role);
    }

private:
    pref_t *pref_;
    module_t *module_;
};

class ModulePrefTreeWidgetItem : public QTreeWidgetItem
{
public:
    ModulePrefTreeWidgetItem(QTreeWidgetItem *parent, module_t *module) :
        QTreeWidgetItem (parent, module_type_),
        module_(module),
        mpsa_(0)
    {}
    void ensureModulePreferencesScrollArea(QStackedWidget *sw) {
        /*
         * We create pages for interior nodes even if they don't have
         * preferences, so that we at least have something to show
         * if the user clicks on them, even if it's empty.
         */

        /* Scrolled window */
        if (!mpsa_) {
            mpsa_ = new ModulePreferencesScrollArea(module_);
            if (sw->indexOf(mpsa_) < 0) sw->addWidget(mpsa_);
        }
    }

    virtual QVariant data(int column, int role) const {
        if (column == 0) {
            switch (role) {
            case Qt::DisplayRole:
                return QString(module_->title);
            case module_name_role_:
                return QString (module_->name);
            case mpsa_role_:
                return qVariantFromValue(mpsa_);
            default:
                break;
            }
        }
        return QTreeWidgetItem::data(column, role);
    }

private:
    module_t *module_;
    QWidget *mpsa_;
};

extern "C" {
// Callbacks prefs routines

static guint
fill_advanced_prefs(module_t *module, gpointer root_ptr)
{
    QTreeWidgetItem *root_item = static_cast<QTreeWidgetItem *>(root_ptr);

    if (!module || !root_item) return 1;

    if (module->numprefs < 1 && !prefs_module_has_submodules(module)) return 0;

    QString module_title = module->title;

    QTreeWidgetItem *tl_item = new QTreeWidgetItem(root_item);
    tl_item->setText(0, module_title);
    tl_item->setToolTip(0, QString("<span>%1</span>").arg(module->description));
    tl_item->setFirstColumnSpanned(true);
    Qt::ItemFlags item_flags = tl_item->flags();
    item_flags &= ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    tl_item->setFlags(item_flags);


    QList<QTreeWidgetItem *>tl_children;
    for (GList *pref_l = module->prefs; pref_l && pref_l->data; pref_l = g_list_next(pref_l)) {
        pref_t *pref = (pref_t *) pref_l->data;

        if (prefs_get_type(pref) == PREF_OBSOLETE || prefs_get_type(pref) == PREF_STATIC_TEXT) continue;

        const char *type_name = prefs_pref_type_name(pref);
        if (!type_name) continue;

        pref_stash(pref, NULL);

        AdvancedPrefTreeWidgetItem *item = new AdvancedPrefTreeWidgetItem(pref, module);
        tl_children << item;

        // .uat is a void * so it wins the "useful key value" prize.
        if (prefs_get_uat_value(pref)) {
            prefInsertPrefPtr( prefs_get_uat_value(pref), pref);
        }
    }
    tl_item->addChildren(tl_children);

    if(prefs_module_has_submodules(module))
        return prefs_modules_foreach_submodules(module, fill_advanced_prefs, tl_item);

    return 0;
}

static guint
pref_exists(pref_t *, gpointer)
{
    return 1;
}

static guint
fill_module_prefs(module_t *module, gpointer ti_ptr)
{
    QTreeWidgetItem *item = static_cast<QTreeWidgetItem *>(ti_ptr);

    if (!item) return 0;

    QStackedWidget *stacked_widget = VariantPointer<QStackedWidget>::asPtr(item->data(0, stacked_role_));

    if (!stacked_widget) return 0;

    if (!module->use_gui) {
        /* This module uses its own GUI interface to modify its
         * preferences, so ignore it
         */
        return 0;
    }

    /*
     * Is this module an interior node, with modules underneath it?
     */
    if (!prefs_module_has_submodules(module)) {
        /*
         * No.
         * Does it have any preferences (other than possibly obsolete ones)?
         */
        if (prefs_pref_foreach(module, pref_exists, NULL) == 0) {
            /*
             * No.  Don't put the module into the preferences window,
             * as there's nothing to show.
             *
             * XXX - we should do the same for interior ndes; if the module
             * has no non-obsolete preferences *and* nothing under it has
             * non-obsolete preferences, don't put it into the window.
             */
            return 0;
        }
    }

    /*
     * Add this module to the tree.
     */
    ModulePrefTreeWidgetItem *new_mpti = new ModulePrefTreeWidgetItem(item, module);
    new_mpti->setData(0, stacked_role_, item->data(0, stacked_role_));

    /*
     * Is this an interior node?
     */
    if (prefs_module_has_submodules(module)) {
        /*
         * Yes. Walk the subtree and attach stuff to it.
         */
        prefs_modules_foreach_submodules(module, fill_module_prefs, (gpointer) new_mpti);
    }

    return 0;
}

static guint
module_prefs_unstash(module_t *module, gpointer data)
{
    gboolean *must_redissect_p = (gboolean *)data;
    pref_unstash_data_t unstashed_data;

    unstashed_data.handle_decode_as = TRUE;

    module->prefs_changed = FALSE;        /* assume none of them changed */
    for (GList *pref_l = module->prefs; pref_l && pref_l->data; pref_l = g_list_next(pref_l)) {
        pref_t *pref = (pref_t *) pref_l->data;

        if (prefs_get_type(pref) == PREF_OBSOLETE || prefs_get_type(pref) == PREF_STATIC_TEXT) continue;

        unstashed_data.module = module;
        pref_unstash(pref, &unstashed_data);
    }

    /* If any of them changed, indicate that we must redissect and refilter
       the current capture (if we have one), as the preference change
       could cause packets to be dissected differently. */
    if (module->prefs_changed)
        *must_redissect_p = TRUE;

    if(prefs_module_has_submodules(module))
        return prefs_modules_foreach_submodules(module, module_prefs_unstash, data);

    return 0;     /* Keep unstashing. */
}

static guint
module_prefs_clean_stash(module_t *module, gpointer)
{
    for (GList *pref_l = module->prefs; pref_l && pref_l->data; pref_l = g_list_next(pref_l)) {
        pref_t *pref = (pref_t *) pref_l->data;

        if (prefs_get_type(pref) == PREF_OBSOLETE || prefs_get_type(pref) == PREF_STATIC_TEXT) continue;

        pref_clean_stash(pref, NULL);
    }

    if(prefs_module_has_submodules(module))
        return prefs_modules_foreach_submodules(module, module_prefs_clean_stash, NULL);

    return 0;     /* Keep cleaning modules */
}

} // extern "C"

// Preference tree items
const int appearance_item_ = 0;
const int capture_item_    = 1;

PreferencesDialog::PreferencesDialog(QWidget *parent) :
    GeometryStateDialog(parent),
    pd_ui_(new Ui::PreferencesDialog),
    cur_pref_type_(0),
    cur_line_edit_(NULL),
    cur_combo_box_(NULL),
    saved_combo_idx_(0)
{
    QTreeWidgetItem tmp_item; // Adding pre-populated top-level items is much faster
    prefs_modules_foreach_submodules(NULL, fill_advanced_prefs, (gpointer) &tmp_item);

    // Some classes depend on pref_ptr_to_pref_ so this MUST be called after
    // fill_advanced_prefs.
    pd_ui_->setupUi(this);
    loadGeometry();

    setWindowTitle(wsApp->windowTitleString(tr("Preferences")));
    pd_ui_->advancedTree->invisibleRootItem()->addChildren(tmp_item.takeChildren());

    pd_ui_->splitter->setStretchFactor(0, 1);
    pd_ui_->splitter->setStretchFactor(1, 5);

    pd_ui_->prefsTree->invisibleRootItem()->child(appearance_item_)->setExpanded(true);

    bool disable_capture = true;
#ifdef HAVE_LIBPCAP
#ifdef _WIN32
    /* Is WPcap loaded? */
    if (has_wpcap) {
#endif /* _WIN32 */
        disable_capture = false;
#ifdef _WIN32
    }
#endif /* _WIN32 */
#endif /* HAVE_LIBPCAP */
    pd_ui_->prefsTree->invisibleRootItem()->child(capture_item_)->setDisabled(disable_capture);

    // PreferencesPane, prefsTree, and stackedWidget must all correspond to each other.
    // This may not be the best way to go about enforcing that.
    QTreeWidgetItem *item = pd_ui_->prefsTree->topLevelItem(0);
    item->setSelected(true);
    pd_ui_->stackedWidget->setCurrentIndex(0);
    for (int i = 0; i < pd_ui_->stackedWidget->count() && item; i++) {
        item->setData(0, mpsa_role_, qVariantFromValue(pd_ui_->stackedWidget->widget(i)));
        item = pd_ui_->prefsTree->itemBelow(item);
    }
    item = pd_ui_->prefsTree->topLevelItem(0);
    prefs_pane_to_item_[ppAppearance] = item;
    prefs_pane_to_item_[ppLayout] = item->child(0);
    prefs_pane_to_item_[ppColumn] = item->child(1);
    prefs_pane_to_item_[ppFontAndColor] = item->child(2);
    prefs_pane_to_item_[ppCapture] = pd_ui_->prefsTree->topLevelItem(1);
    prefs_pane_to_item_[ppFilterExpressions] = pd_ui_->prefsTree->topLevelItem(2);

    pd_ui_->filterExpressonsFrame->setUat(uat_get_table_by_name("Display expressions"));

    // Printing prefs don't apply here.
    module_t *print_module = prefs_find_module("print");
    if (print_module)
        print_module->use_gui = FALSE;

    //Since "expert" is really a pseudo protocol, it shouldn't be
    //categorized with other "real" protocols when it comes to
    //preferences.  Since it's just a UAT, don't bury it in
    //with the other protocols
    pd_ui_->expertFrame->setUat(uat_get_table_by_name("Expert Info Severity Level Configuration"));
    module_t *expert_module = prefs_find_module("_ws.expert");
    if (expert_module)
       expert_module->use_gui = FALSE;


    // We called takeChildren above so this shouldn't be necessary.
    while (tmp_item.childCount() > 0) {
        tmp_item.removeChild(tmp_item.child(0));
    }
    tmp_item.setData(0, stacked_role_, VariantPointer<QStackedWidget>::asQVariant(pd_ui_->stackedWidget));
    prefs_modules_foreach_submodules(NULL, fill_module_prefs, (gpointer) &tmp_item);
    pd_ui_->prefsTree->invisibleRootItem()->insertChildren(
                pd_ui_->prefsTree->invisibleRootItem()->childCount() - 1, tmp_item.takeChildren());
}

PreferencesDialog::~PreferencesDialog()
{
    delete pd_ui_;
    prefs_modules_foreach_submodules(NULL, module_prefs_clean_stash, NULL);
}

void PreferencesDialog::setPane(PreferencesDialog::PreferencesPane start_pane)
{
    if (prefs_pane_to_item_.contains(start_pane)) {
        pd_ui_->prefsTree->setCurrentItem(prefs_pane_to_item_[start_pane]);
    }
}

// Only valid for ModulePrefTreeWidgetItems.
void PreferencesDialog::setPane(const QString module_name)
{
    QTreeWidgetItemIterator pref_it(pd_ui_->prefsTree);
    while (*pref_it) {
        if ((*pref_it)->type() == module_type_) {
            ModulePrefTreeWidgetItem *mp_ti = dynamic_cast<ModulePrefTreeWidgetItem *>(*pref_it);
            // Ensure that the module's scroll area exists and that it's in the
            // widget stack.
            if (mp_ti) {
                QString mpsa_name = (*pref_it)->data(0, module_name_role_).toString();
                if (mpsa_name == module_name) {
                    mp_ti->ensureModulePreferencesScrollArea(pd_ui_->stackedWidget);
                    QWidget *mpsa = (*pref_it)->data(0, mpsa_role_).value<QWidget *>();
                    if (mpsa) {
                        pd_ui_->prefsTree->setCurrentItem((*pref_it));
                        break;
                    }
                }
            }
        }
        ++pref_it;
    }
}

void PreferencesDialog::showEvent(QShowEvent *)
{
    QStyleOption style_opt;
    int new_prefs_tree_width =  pd_ui_->prefsTree->style()->subElementRect(QStyle::SE_TreeViewDisclosureItem, &style_opt).left();
    QList<int> sizes = pd_ui_->splitter->sizes();

#ifdef Q_OS_WIN
    new_prefs_tree_width *= 2;
#endif
    pd_ui_->prefsTree->resizeColumnToContents(0);
    new_prefs_tree_width += pd_ui_->prefsTree->columnWidth(0);
    pd_ui_->prefsTree->setMinimumWidth(new_prefs_tree_width);
    sizes[1] += sizes[0] - new_prefs_tree_width;
    sizes[0] = new_prefs_tree_width;
    pd_ui_->splitter->setSizes(sizes);
    pd_ui_->splitter->setStretchFactor(0, 1);

    pd_ui_->advancedTree->expandAll();
    pd_ui_->advancedTree->setSortingEnabled(true);
    pd_ui_->advancedTree->sortByColumn(0, Qt::AscendingOrder);

    int one_em = fontMetrics().height();
    pd_ui_->advancedTree->setColumnWidth(adv_name_col_, one_em * 12); // Don't let long items widen things too much
    pd_ui_->advancedTree->resizeColumnToContents(adv_status_col_);
    pd_ui_->advancedTree->resizeColumnToContents(adv_type_col_);
    pd_ui_->advancedTree->setColumnWidth(adv_value_col_, one_em * 30);
}

void PreferencesDialog::keyPressEvent(QKeyEvent *evt)
{
    if (cur_line_edit_ && cur_line_edit_->hasFocus()) {
        switch (evt->key()) {
        case Qt::Key_Escape:
            cur_line_edit_->setText(saved_string_pref_);
            /* Fall Through */
        case Qt::Key_Enter:
        case Qt::Key_Return:
            switch (cur_pref_type_) {
            case PREF_UINT:
                uintPrefEditingFinished();
                break;
            case PREF_STRING:
                stringPrefEditingFinished();
                break;
            case PREF_RANGE:
                rangePrefEditingFinished();
                break;
            default:
                break;
            }

            delete cur_line_edit_;
            return;
        default:
            break;
        }
    } else if (cur_combo_box_ && cur_combo_box_->hasFocus()) {
        switch (evt->key()) {
        case Qt::Key_Escape:
            cur_combo_box_->setCurrentIndex(saved_combo_idx_);
            /* Fall Through */
        case Qt::Key_Enter:
        case Qt::Key_Return:
            // XXX The combo box eats enter and return
            enumPrefCurrentIndexChanged(cur_combo_box_->currentIndex());
            delete cur_combo_box_;
            return;
        default:
            break;
        }
    }
    QDialog::keyPressEvent(evt);
}

void PreferencesDialog::on_prefsTree_currentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *)
{
    if (!current) return;
    QWidget *new_item = NULL;

    // "current" might be a QTreeWidgetItem from our .ui file, e.g. "Columns"
    // or a ModulePrefTreeWidgetItem created by fill_module_prefs, e.g. a
    // protocol preference. If it's the latter, ensure that the module's
    // scroll area exists and that it's in the widget stack.
    if (current->type() == module_type_) {
        ModulePrefTreeWidgetItem *mp_ti = dynamic_cast<ModulePrefTreeWidgetItem *>(current);
        if (mp_ti) mp_ti->ensureModulePreferencesScrollArea(pd_ui_->stackedWidget);
    }

    new_item = current->data(0, mpsa_role_).value<QWidget *>();
    g_assert(new_item != NULL);
    pd_ui_->stackedWidget->setCurrentWidget(new_item);
}

void PreferencesDialog::on_advancedSearchLineEdit_textEdited(const QString &search_re)
{
    QTreeWidgetItemIterator branch_it(pd_ui_->advancedTree);
    QRegExp regex(search_re, Qt::CaseInsensitive);

    // Hide or show everything
    while (*branch_it) {
        (*branch_it)->setHidden(!search_re.isEmpty());
        ++branch_it;
    }
    if (search_re.isEmpty()) return;

    // Hide or show each item, showing its parents if needed
    QTreeWidgetItemIterator pref_it(pd_ui_->advancedTree);
    while (*pref_it) {
        bool hidden = true;

        if ((*pref_it)->type() == advanced_type_) {

            if ((*pref_it)->text(0).contains(regex) ||
                (*pref_it)->toolTip(0).contains(regex)) {
                hidden = false;
            }

            (*pref_it)->setHidden(hidden);
            if (!hidden) {
                QTreeWidgetItem *parent = (*pref_it)->parent();
                while (parent) {
                    parent->setHidden(false);
                    parent = parent->parent();
                }
            }
        }
        ++pref_it;
    }
}

void PreferencesDialog::on_advancedTree_currentItemChanged(QTreeWidgetItem *, QTreeWidgetItem *previous)
{
    if (previous && pd_ui_->advancedTree->itemWidget(previous, 3)) {
        pd_ui_->advancedTree->removeItemWidget(previous, 3);
    }
}

void PreferencesDialog::on_advancedTree_itemActivated(QTreeWidgetItem *item, int column)
{
    AdvancedPrefTreeWidgetItem *adv_ti;
    pref_t *pref = NULL;

    if (item->type() == advanced_type_) {
        adv_ti = dynamic_cast<AdvancedPrefTreeWidgetItem *>(item);
        if (adv_ti) pref = adv_ti->pref();
    }

    if (!pref || cur_line_edit_ || cur_combo_box_) return;

    if (column < 3) { // Reset to default
        reset_stashed_pref(pref);
        adv_ti->updatePref();
    } else {
        QWidget *editor = NULL;

        switch (prefs_get_type(pref)) {
        case PREF_DECODE_AS_UINT:
        case PREF_UINT:
        {
            char* tmpstr = prefs_pref_to_str(pref, pref_stashed);
            cur_line_edit_ = new QLineEdit();
//            cur_line_edit_->setInputMask("0000000009;");
            saved_string_pref_ = tmpstr;
            g_free(tmpstr);
            connect(cur_line_edit_, SIGNAL(editingFinished()), this, SLOT(uintPrefEditingFinished()));
            editor = cur_line_edit_;
            break;
        }
        case PREF_BOOL:
            prefs_invert_bool_value(pref, pref_stashed);
            adv_ti->updatePref();
            break;
        case PREF_ENUM:
        {
            cur_combo_box_ = new QComboBox();
            const enum_val_t *ev;
            for (ev = prefs_get_enumvals(pref); ev && ev->description; ev++) {
                cur_combo_box_->addItem(ev->description, QVariant(ev->value));
                if (prefs_get_enum_value(pref, pref_stashed) == ev->value)
                    cur_combo_box_->setCurrentIndex(cur_combo_box_->count() - 1);
            }
            saved_combo_idx_ = cur_combo_box_->currentIndex();
            connect(cur_combo_box_, SIGNAL(currentIndexChanged(int)), this, SLOT(enumPrefCurrentIndexChanged(int)));
            editor = cur_combo_box_;
            break;
        }
        case PREF_STRING:
        {
            cur_line_edit_ = new QLineEdit();
            saved_string_pref_ = prefs_get_string_value(pref, pref_stashed);
            connect(cur_line_edit_, SIGNAL(editingFinished()), this, SLOT(stringPrefEditingFinished()));
            editor = cur_line_edit_;
            break;
        }
        case PREF_SAVE_FILENAME:
        case PREF_OPEN_FILENAME:
        case PREF_DIRNAME:
        {
            QString filename;

            if (prefs_get_type(pref) == PREF_SAVE_FILENAME) {
                filename = QFileDialog::getSaveFileName(this, wsApp->windowTitleString(prefs_get_title(pref)),
                                                        prefs_get_string_value(pref, pref_stashed));

            } else if (prefs_get_type(pref) == PREF_OPEN_FILENAME) {
                filename = QFileDialog::getOpenFileName(this, wsApp->windowTitleString(prefs_get_title(pref)),
                                                        prefs_get_string_value(pref, pref_stashed));

            } else {
                filename = QFileDialog::getExistingDirectory(this, wsApp->windowTitleString(prefs_get_title(pref)),
                                                        prefs_get_string_value(pref, pref_stashed));
            }

            if (!filename.isEmpty()) {
                prefs_set_string_value(pref, QDir::toNativeSeparators(filename).toStdString().c_str(), pref_stashed);
                adv_ti->updatePref();
            }
            break;
        }
        case PREF_DECODE_AS_RANGE:
        case PREF_RANGE:
        {
            SyntaxLineEdit *syntax_edit = new SyntaxLineEdit();
            char *cur_val = prefs_pref_to_str(pref, pref_stashed);
            saved_string_pref_ = gchar_free_to_qstring(cur_val);
            connect(syntax_edit, SIGNAL(textChanged(QString)),
                    this, SLOT(rangePrefTextChanged(QString)));
            connect(syntax_edit, SIGNAL(editingFinished()), this, SLOT(rangePrefEditingFinished()));
            editor = cur_line_edit_ = syntax_edit;
            break;
        }
        case PREF_COLOR:
        {
            QColorDialog color_dlg;
            color_t color = *prefs_get_color_value(pref, pref_stashed);

            color_dlg.setCurrentColor(QColor(
                                          color.red >> 8,
                                          color.green >> 8,
                                          color.blue >> 8
                                          ));
            if (color_dlg.exec() == QDialog::Accepted) {
                QColor cc = color_dlg.currentColor();
                color.red = cc.red() << 8 | cc.red();
                color.green = cc.green() << 8 | cc.green();
                color.blue = cc.blue() << 8 | cc.blue();
                prefs_set_color_value(pref, color, pref_stashed);
                adv_ti->updatePref();
            }
            break;
        }
        case PREF_UAT:
        {
            if (prefs_get_gui_type(pref) == GUI_ALL || prefs_get_gui_type(pref) == GUI_QT) {
                UatDialog uat_dlg(this, prefs_get_uat_value(pref));
                uat_dlg.exec();
            }
            break;
        }
        default:
            break;
        }
        cur_pref_type_ = prefs_get_type(pref);
        if (cur_line_edit_) {
            cur_line_edit_->setText(saved_string_pref_);
            cur_line_edit_->selectAll();
            connect(cur_line_edit_, SIGNAL(destroyed()), this, SLOT(lineEditPrefDestroyed()));
        }
        if (cur_combo_box_) {
            connect(cur_combo_box_, SIGNAL(destroyed()), this, SLOT(enumPrefDestroyed()));
        }
        if (editor) {
            QFrame *edit_frame = new QFrame();
            QHBoxLayout *hb = new QHBoxLayout();
            QSpacerItem *spacer = new QSpacerItem(5, 10);

            hb->addWidget(editor, 0);
            hb->addSpacerItem(spacer);
            hb->setStretch(1, 1);
            hb->setContentsMargins(0, 0, 0, 0);

            edit_frame->setLineWidth(0);
            edit_frame->setFrameStyle(QFrame::NoFrame);
            // The documentation suggests setting autoFillbackground. That looks silly
            // so we clear the item text instead.
            item->setText(3, "");
            edit_frame->setLayout(hb);
            pd_ui_->advancedTree->setItemWidget(item, 3, edit_frame);
            editor->setFocus();
        }
    }
}

void PreferencesDialog::lineEditPrefDestroyed()
{
    cur_line_edit_ = NULL;
}

void PreferencesDialog::enumPrefDestroyed()
{
    cur_combo_box_ = NULL;
}

void PreferencesDialog::uintPrefEditingFinished()
{
    AdvancedPrefTreeWidgetItem *adv_ti = dynamic_cast<AdvancedPrefTreeWidgetItem *>(pd_ui_->advancedTree->currentItem());
    if (!cur_line_edit_ || !adv_ti) return;

    pref_t *pref = adv_ti->pref();
    if (!pref) return;

    bool ok;
    guint new_val = cur_line_edit_->text().toUInt(&ok, prefs_get_uint_base(pref));

    if (ok) prefs_set_uint_value(pref, new_val, pref_stashed);
    pd_ui_->advancedTree->removeItemWidget(adv_ti, 3);
    adv_ti->updatePref();
}

void PreferencesDialog::enumPrefCurrentIndexChanged(int index)
{
    AdvancedPrefTreeWidgetItem *adv_ti = dynamic_cast<AdvancedPrefTreeWidgetItem *>(pd_ui_->advancedTree->currentItem());
    if (!cur_combo_box_ || !adv_ti || index < 0) return;

    pref_t *pref = adv_ti->pref();
    if (!pref) return;

    prefs_set_uint_value(pref, cur_combo_box_->itemData(index).toInt(), pref_stashed);
    adv_ti->updatePref();
}

void PreferencesDialog::stringPrefEditingFinished()
{
    AdvancedPrefTreeWidgetItem *adv_ti = dynamic_cast<AdvancedPrefTreeWidgetItem *>(pd_ui_->advancedTree->currentItem());
    if (!cur_line_edit_ || !adv_ti) return;

    pref_t *pref = adv_ti->pref();
    if (!pref) return;

    prefs_set_string_value(pref, cur_line_edit_->text().toStdString().c_str(), pref_stashed);
    pd_ui_->advancedTree->removeItemWidget(adv_ti, 3);
    adv_ti->updatePref();
}

void PreferencesDialog::rangePrefTextChanged(const QString &text)
{
    SyntaxLineEdit *syntax_edit = qobject_cast<SyntaxLineEdit *>(cur_line_edit_);
    AdvancedPrefTreeWidgetItem *adv_ti = dynamic_cast<AdvancedPrefTreeWidgetItem *>(pd_ui_->advancedTree->currentItem());
    if (!syntax_edit || !adv_ti) return;

    pref_t *pref = adv_ti->pref();
    if (!pref) return;

    if (text.isEmpty()) {
        syntax_edit->setSyntaxState(SyntaxLineEdit::Empty);
    } else {
        range_t *newrange;
        convert_ret_t ret = range_convert_str(NULL, &newrange, text.toUtf8().constData(), prefs_get_max_value(pref));

        if (ret == CVT_NO_ERROR) {
            syntax_edit->setSyntaxState(SyntaxLineEdit::Valid);
        } else {
            syntax_edit->setSyntaxState(SyntaxLineEdit::Invalid);
        }
        wmem_free(NULL, newrange);
    }
}

void PreferencesDialog::rangePrefEditingFinished()
{
    SyntaxLineEdit *syntax_edit = qobject_cast<SyntaxLineEdit *>(QObject::sender());
    AdvancedPrefTreeWidgetItem *adv_ti = dynamic_cast<AdvancedPrefTreeWidgetItem *>(pd_ui_->advancedTree->currentItem());
    if (!syntax_edit || !adv_ti) return;

    pref_t *pref = adv_ti->pref();
    if (!pref) return;

    prefs_set_stashed_range_value(pref, syntax_edit->text().toUtf8().constData());
    pd_ui_->advancedTree->removeItemWidget(adv_ti, 3);
    adv_ti->updatePref();
}

void PreferencesDialog::on_buttonBox_accepted()
{
    gchar* err = NULL;
    gboolean must_redissect = FALSE;

    QVector<unsigned> old_layout = QVector<unsigned>() << prefs.gui_layout_type
                                                       << prefs.gui_layout_content_1
                                                       << prefs.gui_layout_content_2
                                                       << prefs.gui_layout_content_3;

    // XXX - We should validate preferences as the user changes them, not here.
    // XXX - We're also too enthusiastic about setting must_redissect.
//    if (!prefs_main_fetch_all(parent_w, &must_redissect))
//        return; /* Errors in some preference setting - already reported */
    prefs_modules_foreach_submodules(NULL, module_prefs_unstash, (gpointer) &must_redissect);

    QVector<unsigned> new_layout = QVector<unsigned>() << prefs.gui_layout_type
                                                       << prefs.gui_layout_content_1
                                                       << prefs.gui_layout_content_2
                                                       << prefs.gui_layout_content_3;

    if (new_layout[0] != old_layout[0]) {
        // Layout type changed, reset sizes
        recent.gui_geometry_main_upper_pane = 0;
        recent.gui_geometry_main_lower_pane = 0;
    }

    pd_ui_->columnFrame->unstash();
    pd_ui_->filterExpressonsFrame->acceptChanges();
    pd_ui_->expertFrame->acceptChanges();

    prefs_main_write();
    if (save_decode_as_entries(&err) < 0)
    {
        simple_dialog(ESD_TYPE_ERROR, ESD_BTN_OK, "%s", err);
        g_free(err);
    }

    write_language_prefs();
    wsApp->loadLanguage(QString(language));

#ifdef HAVE_AIRPCAP
  /*
   * Load the Wireshark decryption keys (just set) and save
   * the changes to the adapters' registry
   */
  //airpcap_load_decryption_keys(airpcap_if_list);
#endif

    // gtk/prefs_dlg.c:prefs_main_apply_all
    /*
     * Apply the protocol preferences first - "gui_prefs_apply()" could
     * cause redissection, and we have to make sure the protocol
     * preference changes have been fully applied.
     */
    prefs_apply_all();

    /* Fill in capture options with values from the preferences */
    prefs_to_capture_opts();

#ifdef HAVE_AIRPCAP
//    prefs_airpcap_update();
#endif

    wsApp->setMonospaceFont(prefs.gui_qt_font_name);

    if (must_redissect) {
        /* Redissect all the packets, and re-evaluate the display filter. */
        wsApp->queueAppSignal(WiresharkApplication::PacketDissectionChanged);
    }
    wsApp->queueAppSignal(WiresharkApplication::PreferencesChanged);

    if (new_layout != old_layout) {
        wsApp->queueAppSignal(WiresharkApplication::RecentPreferencesRead);
    }
}

void PreferencesDialog::on_buttonBox_rejected()
{
    //handle frames that don't have their own OK/Cancel "buttons"
    pd_ui_->filterExpressonsFrame->rejectChanges();
    pd_ui_->expertFrame->rejectChanges();
}

void PreferencesDialog::on_buttonBox_helpRequested()
{
    wsApp->helpTopicAction(HELP_PREFERENCES_DIALOG);
}

/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
