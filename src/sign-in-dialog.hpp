/*
 * obs-gleem
 * Copyright (C) 2026 Anikeen UG (haftungsbeschränkt) & Co. KG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QTimer;

/**
 * Signs the plugin in to gleem.gg with the OAuth device flow: shows the
 * code, opens /activate in the browser and closes once the creator has
 * approved. Signed in already, it shows who and offers to sign out.
 */
class SignInDialog : public QDialog {
	Q_OBJECT

public:
	explicit SignInDialog(QWidget *parent = nullptr);
	~SignInDialog() override;

	// Opens the dialog modally over OBS's main window.
	static void run();

private:
	void showSignedIn();
	void startSignIn();
	void poll();

	QLabel *message_;
	QLabel *code_;
	QPushButton *primary_;
	QPushButton *copy_;
	QPushButton *close_;
	QTimer *timer_;
	bool browser_opened_ = false;
};
