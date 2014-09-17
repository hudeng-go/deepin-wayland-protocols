/********************************************************************
Copyright 2013  Martin Gräßlin <mgraesslin@kde.org>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) version 3, or any
later version accepted by the membership of KDE e.V. (or its
successor approved by the membership of KDE e.V.), which shall
act as a proxy defined in Section 6 of version 3 of the license.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
#include "shm_pool.h"
#include "buffer.h"
// Qt
#include <QDebug>
#include <QImage>
#include <QTemporaryFile>
// system
#include <unistd.h>
#include <sys/mman.h>

namespace KWin
{
namespace Wayland
{

ShmPool::ShmPool(QObject *parent)
    : QObject(parent)
    , m_shm(nullptr)
    , m_pool(nullptr)
    , m_poolData(nullptr)
    , m_size(1024)
    , m_tmpFile(new QTemporaryFile())
    , m_valid(false)
    , m_offset(0)
{
}

ShmPool::~ShmPool()
{
    release();
}

void ShmPool::release()
{
    qDeleteAll(m_buffers);
    m_buffers.clear();
    if (m_poolData) {
        munmap(m_poolData, m_size);
        m_poolData = nullptr;
    }
    if (m_pool) {
        wl_shm_pool_destroy(m_pool);
        m_pool = nullptr;
    }
    if (m_shm) {
        wl_shm_destroy(m_shm);
        m_shm = nullptr;
    }
    m_tmpFile->close();
    m_valid = false;
    m_offset = 0;
}

void ShmPool::destroy()
{
    qDeleteAll(m_buffers);
    m_buffers.clear();
    if (m_poolData) {
        munmap(m_poolData, m_size);
        m_poolData = nullptr;
    }
    if (m_pool) {
        free(m_pool);
        m_pool = nullptr;
    }
    if (m_shm) {
        free(m_shm);
        m_shm = nullptr;
    }
    m_tmpFile->close();
    m_valid = false;
    m_offset = 0;
}

void ShmPool::setup(wl_shm *shm)
{
    Q_ASSERT(shm);
    Q_ASSERT(!m_shm);
    m_shm = shm;
    m_valid = createPool();
}

bool ShmPool::createPool()
{
    if (!m_tmpFile->open()) {
        qDebug() << "Could not open temporary file for Shm pool";
        return false;
    }
    if (ftruncate(m_tmpFile->handle(), m_size) < 0) {
        qDebug() << "Could not set size for Shm pool file";
        return false;
    }
    m_poolData = mmap(NULL, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_tmpFile->handle(), 0);
    m_pool = wl_shm_create_pool(m_shm, m_tmpFile->handle(), m_size);

    if (!m_poolData || !m_pool) {
        qDebug() << "Creating Shm pool failed";
        return false;
    }
    return true;
}

bool ShmPool::resizePool(int32_t newSize)
{
    if (ftruncate(m_tmpFile->handle(), newSize) < 0) {
        qDebug() << "Could not set new size for Shm pool file";
        return false;
    }
    wl_shm_pool_resize(m_pool, newSize);
    munmap(m_poolData, m_size);
    m_poolData = mmap(NULL, newSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_tmpFile->handle(), 0);
    m_size = newSize;
    if (!m_poolData) {
        qDebug() << "Resizing Shm pool failed";
        return false;
    }
    emit poolResized();
    return true;
}

wl_buffer *ShmPool::createBuffer(const QImage& image)
{
    if (image.isNull() || !m_valid) {
        return NULL;
    }
    Buffer *buffer = getBuffer(image.size(), image.bytesPerLine());
    if (!buffer) {
        return NULL;
    }
    buffer->copy(image.bits());
    return buffer->buffer();
}

wl_buffer *ShmPool::createBuffer(const QSize &size, int32_t stride, const void *src)
{
    if (size.isNull() || !m_valid) {
        return NULL;
    }
    Buffer *buffer = getBuffer(size, stride);
    if (!buffer) {
        return NULL;
    }
    buffer->copy(src);
    return buffer->buffer();
}

Buffer *ShmPool::getBuffer(const QSize &size, int32_t stride)
{
    Q_FOREACH (Buffer *buffer, m_buffers) {
        if (!buffer->isReleased() || buffer->isUsed()) {
            continue;
        }
        if (buffer->size() != size || buffer->stride() != stride) {
            continue;
        }
        buffer->setReleased(false);
        return buffer;
    }
    const int32_t byteCount = size.height() * stride;
    if (m_offset + byteCount > m_size) {
        if (!resizePool(m_size + byteCount)) {
            return NULL;
        }
    }
    // we don't have a buffer which we could reuse - need to create a new one
    wl_buffer *native = wl_shm_pool_create_buffer(m_pool, m_offset, size.width(), size.height(),
                                                  stride, WL_SHM_FORMAT_ARGB8888);
    if (!native) {
        return NULL;
    }
    Buffer *buffer = new Buffer(this, native, size, stride, m_offset);
    m_offset += byteCount;
    m_buffers.append(buffer);
    return buffer;
}

}
}