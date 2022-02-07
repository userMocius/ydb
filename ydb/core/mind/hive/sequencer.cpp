
#include "sequencer.h"

namespace NKikimr {
namespace NHive {

TSequencer::TSequence TSequencer::NO_SEQUENCE;

bool TSequenceGenerator::AddFreeSequence(TOwnerType owner, TSequencer::TSequence sequence) {
    Y_VERIFY(sequence.Begin != NO_ELEMENT);
    auto [it, inserted] = SequenceByOwner.emplace(owner, sequence);
    if (inserted) {
        if (!sequence.Empty()) {
            FreeSequences.emplace_back(owner);
        }
        ++FreeSequencesIndex;
    }
    return inserted;
}

void TSequenceGenerator::AddAllocatedSequence(TOwnerType owner, TSequence sequence) {
    Y_VERIFY(owner.first != NO_OWNER);
    Y_VERIFY(sequence.Begin != NO_ELEMENT);
    {
        auto [it, inserted] = AllocatedSequences.emplace(sequence, owner);
        Y_VERIFY(inserted);
    }
    {
        auto [it, inserted] = SequenceByOwner.emplace(owner, sequence);
        Y_VERIFY(inserted);
    }
}

bool TSequenceGenerator::AddSequence(TOwnerType owner, TSequence sequence) {
    if (owner.first == NO_OWNER) {
        return AddFreeSequence(owner, sequence);
    } else {
        AddAllocatedSequence(owner, sequence);
        return true;
    }
}

TSequencer::TElementType TSequenceGenerator::AllocateElement(std::vector<TOwnerType>& modified) {
    return AllocateSequence({NO_OWNER, NO_ELEMENT}, 1, modified).GetNext();
}

TSequencer::TSequence TSequenceGenerator::AllocateSequence(TSequencer::TOwnerType owner, size_t size, std::vector<TOwnerType>& modified) {
    if (size > 1) {
        Y_VERIFY(owner.first != NO_OWNER);
        auto it = SequenceByOwner.find(owner);
        if (it != SequenceByOwner.end()) {
            return it->second;
        }
    }
    if (FreeSequences.empty()) {
        return NO_SEQUENCE;
    }
    TOwnerType freeOwner = FreeSequences.front();
    auto itSequence = SequenceByOwner.find(freeOwner);
    if (itSequence == SequenceByOwner.end()) {
        return NO_SEQUENCE;
    }
    TSequence result = itSequence->second.BiteOff(size);
    if (itSequence->second.Empty()) {
        FreeSequences.pop_front();
    }
    if (size > 1) {
        AddAllocatedSequence(owner, result);
        modified.emplace_back(owner);
    }
    modified.emplace_back(freeOwner);
    return result;
}

TSequencer::TSequence TSequenceGenerator::GetSequence(TSequencer::TOwnerType owner) {
    auto it = SequenceByOwner.find(owner);
    if (it != SequenceByOwner.end()) {
        return it->second;
    } else {
        return NO_SEQUENCE;
    }
}

TSequencer::TOwnerType TSequenceGenerator::GetOwner(TSequencer::TElementType element) {
    auto it = AllocatedSequences.lower_bound(element);
    if (it != AllocatedSequences.end()) {
        // check for "equal"
        if (it->first.Contains(element)) {
            return it->second;
        }
    }
    if (it != AllocatedSequences.begin()) {
        --it;
        if (it->first.Contains(element)) {
            return it->second;
        }
    }

    return {NO_OWNER, 0};
}

size_t TSequenceGenerator::FreeSize() const {
    size_t size = 0;
    for (const auto& owner : FreeSequences) {
        auto itSeq = SequenceByOwner.find(owner);
        if (itSeq != SequenceByOwner.end()) {
            size += itSeq->second.Size();
        }
    }
    return size;
}

size_t TSequenceGenerator::AllocatedSequencesSize() const {
    size_t size = 0;
    for (const auto& [seq, owner] : AllocatedSequences) {
        size += seq.Size();
    }
    return size;
}

size_t TSequenceGenerator::AllocatedSequencesCount() const {
    return AllocatedSequences.size();
}

size_t TSequenceGenerator::NextFreeSequenceIndex() const {
    return FreeSequencesIndex;
}

TSequencer::TElementType TSequenceGenerator::GetNextElement() const {
    if (FreeSequences.empty()) {
        return NO_ELEMENT;
    }
    TOwnerType freeOwner = FreeSequences.front();
    auto itSequence = SequenceByOwner.find(freeOwner);
    if (itSequence == SequenceByOwner.end()) {
        return NO_ELEMENT;
    }
    return itSequence->second.GetNext();
}

void TSequenceGenerator::Clear() {
    SequenceByOwner.clear();
    AllocatedSequences.clear();
    FreeSequences.clear();
    FreeSequencesIndex = 0;
}

void TOwnershipKeeper::AddOwnedSequence(TOwnerType owner, TSequence sequence) {
    Y_VERIFY(owner != NO_OWNER);
    Y_VERIFY(sequence.Begin != NO_ELEMENT);
    {
        auto [it, inserted] = OwnedSequences.emplace(sequence, owner);
        Y_VERIFY(inserted || it->second == owner);
    }
}

TOwnershipKeeper::TOwnerType TOwnershipKeeper::GetOwner(TSequencer::TElementType element) {
    auto it = OwnedSequences.lower_bound(element);
    if (it != OwnedSequences.end()) {
        // check for "equal"
        if (it->first.Contains(element)) {
            return it->second;
        }
    }
    if (it != OwnedSequences.begin()) {
        --it;
        if (it->first.Contains(element)) {
            return it->second;
        }
    }

    return NO_OWNER;
}

void TOwnershipKeeper::Clear() {
    OwnedSequences.clear();
}

}
}
