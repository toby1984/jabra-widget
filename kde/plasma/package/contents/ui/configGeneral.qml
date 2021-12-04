/*
 *  Copyright 2013 David Edmundson <davidedmundson@kde.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  2.010-1301, USA.
 */

import QtQuick 2.0
import QtQuick.Controls 2.0
import org.kde.kirigami 2.5 as Kirigami


Kirigami.FormLayout {

    property alias cfg_debug: debugCheckBox.checked
    property alias cfg_notifyStep: notifyStepTextField.text
    property alias cfg_pollingIntervalSeconds: pollingIntervalSecondsTextField.text

    anchors {
        left: parent.left
        right: parent.right
    }

    CheckBox {
        id: debugCheckBox
        Kirigami.FormData.label: i18n("Debug:")
    }

    TextField {
        id: notifyStepTextField
        Kirigami.FormData.label: i18n("Notification Interval (%)")
    }

    TextField {
        id: pollingIntervalSecondsTextField
        Kirigami.FormData.label: i18n("Device Polling Interval (seconds)")
    }
}
