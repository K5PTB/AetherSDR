#pragma once

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

namespace AetherSDR {

class RadioModel;

// Owns the lifecycle of a split TX slice a CAT protocol creates on demand:
//   * async create — learns the new slice id straight from the radio's create
//     ack (no slice-list snapshot/diff, so a reused slice id or a reused heap
//     address can never be mistaken for the slice we made),
//   * removal on split-disable and on client disconnect (destructor),
//   * the enable→disable race — a disable arriving before the created slice
//     materializes still closes it, even if the protocol is destroyed in the
//     meantime (the in-flight create holds its own control block + a RadioModel
//     QPointer, independent of this object's lifetime).
//
// Shared by RigctlProtocol and SmartCatProtocol so this tricky async ordering
// lives — and is fixed — in exactly one place.
//
// Threading: lives on the same (GUI) thread as RadioModel and the owning
// protocol; every model mutation is issued on that thread. A QObject only so the
// create callback can be QPointer-guarded; owned as a protocol member, so its
// lifetime equals the protocol's.
class SplitSliceManager : public QObject {
    Q_OBJECT
public:
    explicit SplitSliceManager(RadioModel* model, QObject* parent = nullptr);
    ~SplitSliceManager() override;

    // Ask the radio to create a TX slice. The new slice's id is learned from the
    // radio's create ack (see sliceId()/owns()), so the caller can adopt it by id
    // — no slice-list snapshot/diff. If remove() arrives first, or this manager is
    // destroyed before the ack, the slice is closed instead of left orphaned.
    // No-op if a create is already in flight or a slice is already owned.
    void create();

    bool pending() const { return m_inflight != nullptr; } // create issued, id unknown
    bool owns()    const { return m_id >= 0; }             // we own a live slice
    int  sliceId() const { return m_id; }                  // owned id, or -1

    // True while the given id is a slice we asked the radio to remove but which
    // has not yet disappeared — callers should skip it when scanning for a slice
    // to reuse/promote (the radio reuses freed ids).
    bool isRemoving(int id) const { return id >= 0 && id == m_removingId; }

    // Remove the slice we created (direct sendCommand; same thread as the model).
    // If the create is still in flight, the slice is removed as soon as its id
    // arrives. No-op if we own nothing.
    void remove();

    // Forget the owned slice without removing it (e.g. ownership handed to the
    // operator's configured VFO B, or the slice vanished out of band).
    void forget();

private:
    void onCreated(int sliceId);   // create ack delivered the new id (manager alive)

    // Control block for one in-flight create. Outlives the manager so a disable-
    // before-materialize (or a disconnect mid-create) still closes the slice.
    struct InFlight {
        QPointer<RadioModel> model;
        bool removeOnArrival = false;
    };

    QPointer<RadioModel> m_model;
    int  m_id{-1};                          // owned slice id, or -1
    int  m_removingId{-1};                  // id asked to remove, until confirmed gone
    std::shared_ptr<InFlight> m_inflight;   // non-null while a create is pending
};

} // namespace AetherSDR
