// SPDX-License-Identifier: GPL-2.0
#include "dive.h"
#include "metrics.h"
#include "divelist.h"
#include "qthelper.h"
#include "imagedownloader.h"
#include <unistd.h>
#include <QString>

#include <QtConcurrent>

static QUrl cloudImageURL(const char *hash)
{
	return QUrl::fromUserInput(QString("https://cloud.subsurface-divelog.org/images/").append(hash));
}

ImageDownloader::ImageDownloader(struct picture *pic)
{
	picture = pic;
	loadFromHash = false;
}

ImageDownloader::~ImageDownloader()
{
	picture_free(picture);
}

void ImageDownloader::load(bool fromHash){
	QUrl url;
	loadFromHash = fromHash;
	if(fromHash)
		url = cloudImageURL(picture->hash);
	else
		url = QUrl::fromUserInput(QString(picture->filename));
	if (url.isValid()) {
		QEventLoop loop;
		QNetworkAccessManager manager;
		QNetworkRequest request(url);
		connect(&manager, SIGNAL(finished(QNetworkReply *)), this, SLOT(saveImage(QNetworkReply *)));
		QNetworkReply *reply = manager.get(request);
		while (reply->isRunning()) {
			loop.processEvents();
			sleep(1);
		}
		delete reply;
	}
}

void ImageDownloader::saveImage(QNetworkReply *reply)
{
	QByteArray imageData = reply->readAll();
	QImage image = QImage();
	image.loadFromData(imageData);
	if (image.isNull()) {
		if (loadFromHash)
			load(false);
		return;
	}
	QCryptographicHash hash(QCryptographicHash::Sha1);
	hash.addData(imageData);
	QString path = QStandardPaths::standardLocations(QStandardPaths::CacheLocation).first();
	QDir dir(path);
	if (!dir.exists())
		dir.mkpath(path);
	QFile imageFile(path.append("/").append(hash.result().toHex()));
	if (imageFile.open(QIODevice::WriteOnly)) {
		QDataStream stream(&imageFile);
		stream.writeRawData(imageData.data(), imageData.length());
		imageFile.waitForBytesWritten(-1);
		imageFile.close();
		add_hash(imageFile.fileName(), hash.result());
		learnHash(picture, hash.result());
	}
	// This should be called to make the picture actually show.
	// Problem is DivePictureModel is not in core.
	// Nevertheless, the image shows when the dive is selected the next time.
	// DivePictureModel::instance()->updateDivePictures();

}

static void loadPicture(struct picture *picture, bool fromHash)
{
	static QSet<QString> queuedPictures;
	static QMutex pictureQueueMutex;

	if (!picture)
		return;
	QMutexLocker locker(&pictureQueueMutex);
	if (queuedPictures.contains(QString(picture->filename))) {
		picture_free(picture);
		return;
	}
	queuedPictures.insert(QString(picture->filename));
	locker.unlock();

	ImageDownloader download(picture);
	download.load(fromHash);
}

SHashedImage::SHashedImage(struct picture *picture) : QImage()
{
	QUrl url = QUrl::fromUserInput(localFilePath(QString(picture->filename)));
	if(url.isLocalFile())
		load(url.toLocalFile());
	if (isNull()) {
		// This did not load anything. Let's try to get the image from other sources
		// Let's try to load it locally via its hash
		QString filename = fileFromHash(picture->hash);
		if (filename.isNull())
			filename = QString(picture->filename);
		if (filename.isNull()) {
			// That didn't produce a local filename.
			// Try the cloud server
			QtConcurrent::run(loadPicture, clone_picture(picture), true);
		} else {
			// Load locally from translated file name
			load(filename);
			if (!isNull()) {
				// Make sure the hash still matches the image file
				QtConcurrent::run(updateHash, clone_picture(picture));
			} else {
				// Interpret filename as URL
				QtConcurrent::run(loadPicture, clone_picture(picture), false);
			}
		}
	} else {
		// We loaded successfully. Now, make sure hash is up to date.
		QtConcurrent::run(hashPicture, clone_picture(picture));
	}
}

