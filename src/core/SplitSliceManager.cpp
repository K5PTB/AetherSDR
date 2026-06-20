#include "SplitSliceManager.h"

#include "models/RadioModel.h"

#include <QString>

namespace AetherSDR {

SplitSliceManager::SplitSliceManager(RadioModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{
    if (m_model) {
        // Keep our view of ownership honest if the slice disappears out of band
        // (operator/GUI/another client closes it, or our own remove() lands):
        // clear the owned id and lift a pending-removal marker once confirmed.
        connect(m_model, &RadioModel::sliceRemoved, this, [this](int id) {
            // A removal WE requested completing: clear the marker and stop. Do NOT
            // also clear m_id — by the time this fires the radio may have reused
            // this freed id for a NEW slice we now own, and clearing m_id would
            // orphan it.
            if (id == m_removingId) { m_removingId = -1; return; }
            // Our owned slice removed out of band (operator / GUI / another
            // client): forget it so we don't later double-remove a stale id.
            if (id == m_id) m_id = -1;
        });
    }
}

SplitSliceManager::~SplitSliceManager()
{
    // Client disconnect / protocol teardown: close a slice we created so it is
    // not left hanging on the radio. Same thread as the model, so a direct
    // sendCommand is safe (no event-loop dependency, unlike a queued post which
    // would be dropped during app shutdown).
    if (m_id >= 0 && m_model)
        m_model->sendCommand(QString("slice remove %1").arg(m_id));
    // If a create is still in flight, mark its control block so the create
    // callback closes the slice when it finally arrives — the block and its
    // RadioModel pointer outlive this object, so this works even mid-shutdown.
    if (m_inflight)
        m_inflight->removeOnArrival = true;
}

void SplitSliceManager::create()
{
    if (m_inflight || m_id >= 0 || !m_model) return;  // already creating / own one

    auto state = std::make_shared<InFlight>();
    state->model = m_model;
    m_inflight = state;

    QPointer<SplitSliceManager> guard(this);
    m_model->addSlice([guard, state](int id) {
        // id < 0 means the create failed (no panadapter, radio rejected, or an
        // unparseable ack). The created slice should be ADOPTED only if it exists
        // AND (a) this manager is still alive and (b) nobody asked to remove it
        // meanwhile. Otherwise close it (if it exists) so it isn't orphaned. The
        // control block + model pointer are captured by value, so this runs
        // correctly even if the manager was destroyed.
        if (id >= 0 && (!guard || state->removeOnArrival)) {
            if (state->model)
                state->model->sendCommand(QString("slice remove %1").arg(id));
            if (guard) guard->onCreated(-1);   // we own nothing — clear pending
            return;
        }
        if (guard) guard->onCreated(id);       // adopt (id>=0) or clear pending (id<0)
    });
}

void SplitSliceManager::onCreated(int sliceId)
{
    m_inflight.reset();                          // create resolved — no longer pending
    m_id = (sliceId >= 0) ? sliceId : -1;        // adopt by id, or own nothing on failure
}

void SplitSliceManager::remove()
{
    if (m_inflight) {
        // Create still in flight — defer removal to the create callback.
        m_inflight->removeOnArrival = true;
        m_inflight.reset();      // we no longer intend to adopt it
        return;
    }
    if (m_id >= 0 && m_model) {
        m_removingId = m_id;     // skip it on re-enable until sliceRemoved confirms
        m_model->sendCommand(QString("slice remove %1").arg(m_id));
    }
    m_id = -1;
}

void SplitSliceManager::forget()
{
    if (m_inflight) m_inflight->removeOnArrival = false;  // let it live, unowned
    m_inflight.reset();
    m_id = -1;
    m_removingId = -1;
}

} // namespace AetherSDR
