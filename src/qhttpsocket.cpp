/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Nathan Osman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 **/

#include <cstring>

#include "qhttpsocket.h"
#include "qhttpsocket_p.h"

QHttpSocketPrivate::QHttpSocketPrivate(QHttpSocket *httpSocket)
    : q(httpSocket),
      error(QHttpSocket::None),
      requestHeadersRead(false),
      responseStatusCode("200 OK"),
      responseHeadersWritten(false)
{
    connect(&socket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(&socket, SIGNAL(bytesWritten(qint64)), q, SIGNAL(bytesWritten(qint64)));
}

void QHttpSocketPrivate::writeResponseHeaders()
{
    QString headers = QString("HTTP/1.0 %1\r\n").arg(responseStatusCode);

    QMap<QString, QString>::const_iterator i = responseHeaders.constBegin();
    while(i != responseHeaders.constEnd()) {
        headers += QString("%1: %2").arg(i.key(), i.value());
        ++i;
    }

    responseHeadersWritten = true;
}

void QHttpSocketPrivate::onReadyRead()
{
    buffer.append(socket.readAll());

    if(!requestHeadersRead) {

        // Check for two successive CRLF sequences in the input
        int index = buffer.indexOf("\r\n\r\n");
        if(index != -1) {

            parseRequestHeaders(buffer.left(index));

            buffer.remove(0, index + 4);
            requestHeadersRead = true;

            Q_EMIT q->requestHeadersParsed();
        }

    } else {
        Q_EMIT q->readyRead();
    }
}

void QHttpSocketPrivate::abortWithError(QHttpSocket::Error socketError)
{
    error = socketError;

    switch(error) {
    case QHttpSocket::MalformedRequestLine:
        q->setErrorString(tr("Malformed request line"));
        break;
    case QHttpSocket::MalformedRequestHeader:
        q->setErrorString(tr("Malformed request header"));
        break;
    case QHttpSocket::InvalidHttpVersion:
        q->setErrorString(tr("Invalid HTTP version"));
        break;
    case QHttpSocket::IncompleteHeader:
        q->setErrorString(tr("Incomplete header received"));
        break;
    }

    Q_EMIT q->errorChanged(error);
}

void QHttpSocketPrivate::parseRequestHeaders(const QString &headers)
{
    // Each line ends with a CRLF
    QStringList parts = headers.split("\r\n");

    // Parse the request line and each of the headers that follow
    parseRequestLine(parts.takeFirst());
    foreach(QString header, parts) {
        parseRequestHeader(header);
    }
}

void QHttpSocketPrivate::parseRequestLine(const QString &line)
{
    QStringList parts = line.split(" ");

    // Ensure that the request line consists of exactly three parts
    if(parts.count() != 3) {
        abortWithError(QHttpSocket::MalformedRequestLine);
        return;
    }

    // Only HTTP versions 1.0 and 1.1 are currently supported
    if(parts[2] != "HTTP/1.0" && parts[2] != "HTTP/1.1") {
        abortWithError(QHttpSocket::InvalidHttpVersion);
        return;
    }

    requestMethod = parts[0];
    requestUri = parts[1];
}

void QHttpSocketPrivate::parseRequestHeader(const QString &header)
{
    // Ensure that the header line contains at least one ":"
    int index = header.indexOf(":");
    if(index == -1) {
        abortWithError(QHttpSocket::MalformedRequestHeader);
        return;
    }

    // Trim each part of the header and add it
    requestHeaders.insert(
        header.left(index).trimmed().toLower(),
        header.mid(index + 1).trimmed()
    );
}

QHttpSocket::QHttpSocket(qintptr socketDescriptor, QObject *parent)
    : QIODevice(parent),
      d(new QHttpSocketPrivate(this))
{
    d->socket.setSocketDescriptor(socketDescriptor);
    setOpenMode(QIODevice::ReadWrite);
}

QHttpSocket::~QHttpSocket()
{
    delete d;
}

void QHttpSocket::close() const
{
    // If the response headers have not yet been written, then do so before closing
    if(!d->responseHeadersWritten) {
        d->writeResponseHeaders();
    }

    d->socket.close();
}

QHttpSocket::Error QHttpSocket::error() const
{
    return d->error;
}

QString QHttpSocket::requestMethod() const
{
    if(!d->requestHeadersRead) {
        qWarning("Request headers have not yet been read");
    }

    return d->requestMethod;
}

QString QHttpSocket::requestUri() const
{
    if(!d->requestHeadersRead) {
        qWarning("Request headers have not yet been read");
    }

    return d->requestUri;
}

QStringList QHttpSocket::requestHeaders() const
{
    if(!d->requestHeadersRead) {
        qWarning("Request headers have not yet been read");
    }

    return d->requestHeaders.keys();
}

QString QHttpSocket::requestHeader(const QString &header) const
{
    if(!d->requestHeadersRead) {
        qWarning("Request headers have not yet been read");
    }

    return d->requestHeaders.value(header.toLower());
}

void QHttpSocket::setResponseStatusCode(const QString &statusCode)
{
    if(d->responseHeadersWritten) {
        qWarning("Response headers have already been written");
    }

    d->responseStatusCode = statusCode;
}

void QHttpSocket::setResponseHeader(const QString &header, const QString &value)
{
    if(d->responseHeadersWritten) {
        qWarning("Response headers have already been written");
    }

    d->responseHeaders.insert(header, value);
}

bool QHttpSocket::isSequential() const
{
    return true;
}

qint64 QHttpSocket::readData(char *data, qint64 maxlen)
{
    // Data can only be read from the socket once the request headers are read
    if(!d->requestHeadersRead) {
        return -1;
    }

    // Ensure that no more than the requested amount or the size of the buffer is read
    qint64 size = qMin(static_cast<qint64>(d->buffer.size()), maxlen);
    memcpy(data, d->buffer.constData(), size);

    // Remove the amount that was read from the buffer
    d->buffer.remove(0, size);

    return size;
}

qint64 QHttpSocket::writeData(const char *data, qint64 len)
{
    // If the response headers have not yet been written, they
    // must immediately be written before the data can be
    if(!d->responseHeadersWritten) {
        d->writeResponseHeaders();
    }

    return d->socket.write(data, len);
}
