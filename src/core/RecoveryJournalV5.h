#ifndef FIDGET_CORE_RECOVERY_JOURNAL_V5_H
#define FIDGET_CORE_RECOVERY_JOURNAL_V5_H

#include "core/RecoveryJournal.h"

namespace fidget {

[[nodiscard]] TunerRecoverySerializationResult
SerializeTunerRecoveryJournalV5(const TunerRecoveryRecord& record);

[[nodiscard]] TunerRecoveryParseResult ParseTunerRecoveryJournalV5(
    const std::string& text);

} // namespace fidget

#endif
