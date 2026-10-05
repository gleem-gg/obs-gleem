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

#include "sign-in-dialog.hpp"

#include "gleem-account.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace

SignInDialog::SignInDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(text("Gleem.SignIn.Title"));
	setMinimumWidth(420);

	message_ = new QLabel(this);
	message_->setWordWrap(true);
	message_->setTextFormat(Qt::PlainText);

	code_ = new QLabel(this);
	code_->setAlignment(Qt::AlignCenter);
	code_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	QFont font = code_->font();
	font.setPointSizeF(font.pointSizeF() * 2.0);
	font.setBold(true);
	font.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
	code_->setFont(font);
	code_->hide();

	primary_ = new QPushButton(this);
	copy_ = new QPushButton(text("Gleem.SignIn.CopyCode"), this);
	copy_->hide();
	close_ = new QPushButton(text("Gleem.SignIn.Close"), this);

	auto *buttons = new QHBoxLayout();
	buttons->addWidget(copy_);
	buttons->addStretch();
	buttons->addWidget(primary_);
	buttons->addWidget(close_);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(message_);
	layout->addSpacing(8);
	layout->addWidget(code_);
	layout->addSpacing(8);
	layout->addLayout(buttons);

	timer_ = new QTimer(this);
	timer_->setInterval(250);
	connect(timer_, &QTimer::timeout, this, &SignInDialog::poll);
	connect(close_, &QPushButton::clicked, this, &QDialog::reject);
	connect(copy_, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(code_->text()); });

	if (GleemAccount::instance().signed_in())
		showSignedIn();
	else
		startSignIn();
}

SignInDialog::~SignInDialog()
{
	// Closing the dialog while waiting abandons that code.
	if (GleemAccount::instance().sign_in_state().phase != SignInState::Phase::Done)
		GleemAccount::instance().cancel_sign_in();
}

void SignInDialog::run()
{
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (QWidget *active = QApplication::activeWindow())
		parent = active;

	SignInDialog dialog(parent);
	dialog.exec();
}

void SignInDialog::showSignedIn()
{
	timer_->stop();
	code_->hide();
	copy_->hide();
	primary_->disconnect();

	GleemAccount &account = GleemAccount::instance();
	if (account.mode() == AuthMode::Environment) {
		message_->setText(text("Gleem.Account.Rental"));
		primary_->hide();
		return;
	}

	const QString name = QString::fromStdString(account.account_name());
	message_->setText(name.isEmpty() ? text("Gleem.Account.SignedIn") : text("Gleem.Account.SignedInAs").arg(name));
	primary_->setText(text("Gleem.SignIn.SignOut"));
	primary_->show();
	connect(primary_, &QPushButton::clicked, this, [this] {
		GleemAccount::instance().sign_out();
		startSignIn();
	});
}

void SignInDialog::startSignIn()
{
	GleemAccount &account = GleemAccount::instance();
	primary_->disconnect();
	code_->hide();
	copy_->hide();

	if (!account.can_sign_in()) {
		message_->setText(text("Gleem.SignIn.NoClient"));
		primary_->hide();
		return;
	}

	browser_opened_ = false;
	message_->setText(text("Gleem.SignIn.Starting"));
	primary_->setText(text("Gleem.SignIn.OpenBrowser"));
	primary_->setEnabled(false);
	primary_->show();
	connect(primary_, &QPushButton::clicked, this, [] {
		const SignInState state = GleemAccount::instance().sign_in_state();
		QDesktopServices::openUrl(QUrl(QString::fromStdString(state.verification_uri_complete)));
	});

	account.start_sign_in();
	timer_->start();
}

void SignInDialog::poll()
{
	const SignInState state = GleemAccount::instance().sign_in_state();

	switch (state.phase) {
	case SignInState::Phase::Idle:
	case SignInState::Phase::Starting:
		break;
	case SignInState::Phase::Waiting:
		code_->setText(QString::fromStdString(state.user_code));
		code_->show();
		copy_->show();
		primary_->setEnabled(true);
		message_->setText(text("Gleem.SignIn.Waiting").arg(QString::fromStdString(state.verification_uri)));
		if (!browser_opened_) {
			browser_opened_ = true;
			QDesktopServices::openUrl(QUrl(QString::fromStdString(state.verification_uri_complete)));
		}
		break;
	case SignInState::Phase::Done:
		timer_->stop();
		accept();
		break;
	case SignInState::Phase::Failed:
		timer_->stop();
		code_->hide();
		copy_->hide();
		message_->setText(QString::fromStdString(state.error));
		primary_->disconnect();
		primary_->setText(text("Gleem.SignIn.TryAgain"));
		primary_->setEnabled(true);
		connect(primary_, &QPushButton::clicked, this, &SignInDialog::startSignIn);
		break;
	}
}
