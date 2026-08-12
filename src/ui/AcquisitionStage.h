#ifndef FIDGET_UI_ACQUISITION_STAGE_H
#define FIDGET_UI_ACQUISITION_STAGE_H

#include "core/TunerControl.h"
#include "ui/Theme.h"

#include <cstdint>
#include <vector>

namespace fidget {

class AcquisitionStage
{
public:
    void Draw(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const Theme& theme);

private:
    int m_selectedChannel = 0;
    int m_referenceChannel = -1;
    int m_parameterDefinitionIndex = 5;
    int m_proposedParameterValue = 200;
    bool m_parameterValueEdited = false;
    std::vector<std::int16_t> m_referenceSamples;
};

} // namespace fidget

#endif
