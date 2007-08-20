/* Copyright (C) 2005-2007, Thorvald Natvig <thorvald@natvig.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "Audio.h"

#include "AudioInput.h"
#include "AudioOutput.h"
#include "Global.h"

QMap<int, ConfigWidgetNew> *ConfigRegistrar::c_qmNew;

ConfigRegistrar::ConfigRegistrar(int priority, ConfigWidgetNew n) {
	if (! c_qmNew)
		c_qmNew = new QMap<int, ConfigWidgetNew>();
	c_qmNew->insert(priority,n);
}

ConfigWidget::ConfigWidget(Settings &st) : s(st) {
}

QString ConfigWidget::title() const {
	return QLatin1String("Missing Title");
}

QIcon ConfigWidget::icon() const {
	return qApp->windowIcon();
}

void ConfigWidget::accept() const {
}

ConfigDialog::ConfigDialog(QWidget *p) : QDialog(p) {
	setupUi(this);
	qlwIcons->setIconSize(QSize(96, 84));

	s = g.s;

	QWidget *w;
	while ((w = qswPages->widget(0)))
		delete w;

	ConfigWidgetNew cwn;
	foreach(cwn, *ConfigRegistrar::c_qmNew) {
		addPage(cwn(s));
	}

        QPushButton *okButton = buttonBox->button(QDialogButtonBox::Ok);
        okButton->setToolTip(tr("Accept changes"));
        okButton->setWhatsThis(tr("This button will accept current settings and return to the application.<br />"
                                  "The settings will be stored to disk when you leave the application."));

        QPushButton *cancelButton = buttonBox->button(QDialogButtonBox::Cancel);
        cancelButton->setToolTip(tr("Reject changes"));
        cancelButton->setWhatsThis(tr("This button will reject all changes and return to the application.<br />"
                                      "The settings will be reset to the previous positions."));

        QPushButton *applyButton = buttonBox->button(QDialogButtonBox::Apply);
        applyButton->setToolTip(tr("Apply changes"));
        applyButton->setWhatsThis(tr("This button will immediately apply all changes."));

        QPushButton *resetButton = buttonBox->button(QDialogButtonBox::Reset);
        resetButton->setToolTip(tr("Undo changes for current page"));
        resetButton->setWhatsThis(tr("This button will revert any changes done on the current page to the most recent applied settings."));

        QPushButton *restoreButton = buttonBox->button(QDialogButtonBox::RestoreDefaults);
        restoreButton->setToolTip(tr("Restore defaults for current page"));
        restoreButton->setWhatsThis(tr("This button will restore the settings for the current page only to their defaults. Other pages will be not be changed.<br />"
        			"To restore all settings to their defaults, you will have to use this button on every page."
  			      ));

}

void ConfigDialog::addPage(ConfigWidget *cw) {
	QDesktopWidget dw;
	QRect ds=dw.availableGeometry();
	QSize ms=cw->minimumSizeHint();
	ms.rwidth() += 128;
	ms.rheight() += 64;
	if ((ms.width() > ds.width()) || (ms.height() > ds.height())) {
		QScrollArea *qsa=new QScrollArea(this);
		qsa->setWidget(cw);
		qswPages->addWidget(qsa);
		qWarning("Widget %s has size %d %d", qPrintable(cw->title()), ms.width(), ms.height());
	} else {
		qswPages->addWidget(cw);
	}
	QListWidgetItem *i = new QListWidgetItem(qlwIcons);
	i->setIcon(cw->icon());
	i->setText(cw->title());
	i->setTextAlignment(Qt::AlignHCenter);
	i->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

	widgets << cw;
	cw->load(g.s);
}

void ConfigDialog::on_qlwIcons_currentItemChanged(QListWidgetItem *current, QListWidgetItem *previous) {
	if (!current)
		current = previous;


	qswPages->setCurrentIndex(qlwIcons->row(current));
}

void ConfigDialog::on_buttonBox_clicked(QAbstractButton *b) {
	ConfigWidget *conf = qobject_cast<ConfigWidget *>(qswPages->currentWidget());
	switch (buttonBox->standardButton(b)) {
		case QDialogButtonBox::Apply:
		{
			apply();
			break;
		}
		case QDialogButtonBox::RestoreDefaults:
			{
			Settings def;
			if (conf)
				conf->load(def);
			break;
			}
		case QDialogButtonBox::Reset:
			{
			if (conf)
				conf->load(g.s);
			break;
		}
		default:
			break;
	}
}

void ConfigDialog::apply() {
	foreach(ConfigWidget *cw, widgets)
	cw->save();

	boost::weak_ptr<AudioInput> wai(g.ai);
	boost::weak_ptr<AudioOutput> wao(g.ao);

	g.ai.reset();
	g.ao.reset();

	while (! wai.expired() || ! wao.expired()) {
		// Where is QThread::yield() ?
	}

	g.s = s;

	foreach(ConfigWidget *cw, widgets)
		cw->accept();

	g.ai = AudioInputRegistrar::newFromChoice(g.s.qsAudioInput);
	g.ai->start(QThread::HighestPriority);
	g.ao = AudioOutputRegistrar::newFromChoice(g.s.qsAudioOutput);
	g.ao->start(QThread::HighPriority);

	// They might have changed their keys.
	g.iPushToTalk = 0;
	g.iAltSpeak = 0;
}

void ConfigDialog::accept() {
	apply();
	QDialog::accept();
}
