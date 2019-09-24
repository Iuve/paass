/** \file TemplateProcessor.cpp
 *\brief A Template processor class that can be used to build your own.
 *\author S. V. Paulauskas
 *\date October 26, 2014
 */
#include <iostream>

#include "DammPlotIds.hpp"
#include "DetectorSummary.hpp"
#include "RawEvent.hpp"
#include "LearningProcessor.hpp"

namespace dammIds {
    namespace experiment {
        const int D_ENERGY = 1; //!< ID for the energy of the template detector
        //const int DD_TEMPLATE_VS_PULSER = 1; //!< Energy Template vs. Energy Pulser
    }
}//namespace dammIds

using namespace std;
using namespace dammIds::experiment;

LearningProcessor::LearningProcessor() : EventProcessor(OFFSET, RANGE, "LearningProcessor") {
    associatedTypes.insert("learn");
    //histSize_ = RANGE;
}

LearningProcessor::LearningProcessor(double threshold) : EventProcessor(OFFSET, RANGE, "LearningProcessor") {
    associatedTypes.insert("learn");
    threshold_ = threshold;
} 

void LearningProcessor::DeclarePlots(void) {
    DeclareHistogram1D(D_ENERGY, SE, "Energy of the first NaI");
    DeclareHistogram1D(D_ENERGY+1, SE, "Energy of the second NaI");
    DeclareHistogram1D(D_ENERGY+2, SE, "Total energy");
    //DeclareHistogram2D(DD_TEMPLATE_VS_PULSER, SA, SA, "Template Energy vs. Pulser Energy");
}

bool LearningProcessor::PreProcess(RawEvent &event) {
    if (!EventProcessor::PreProcess(event))
        return false;

    evts_ = event.GetSummary("learn")->GetList();
    
    return true;
}

bool LearningProcessor::Process(RawEvent &event) {
    if (!EventProcessor::Process(event))
        return false;
    
    double totalEnergy(0.);
    
    for (vector<ChanEvent *>::const_iterator it = evts_.begin(); it != evts_.end(); it++) {  
		if ((*it)->GetCalibratedEnergy() < threshold_)
			return true;
        totalEnergy += (*it)->GetCalibratedEnergy();
    }
        
    for (vector<ChanEvent *>::const_iterator it = evts_.begin(); it != evts_.end(); it++) {
        unsigned int channelNumber = (*it)->GetChannelNumber();
        
        plot(D_ENERGY+10, totalEnergy);
        if (channelNumber == 0)
            plot(D_ENERGY, (*it)->GetCalibratedEnergy());
        else if (channelNumber == 1)
			plot(D_ENERGY+1, (*it)->GetCalibratedEnergy());
    }

    //static const vector<ChanEvent *> &pulserEvents = event.GetSummary("pulser")->GetList();

   /* for (vector<ChanEvent *>::const_iterator it = evts_.begin(); it != evts_.end(); it++) {
        unsigned int loc = (*it)->GetChanID().GetLocation();
        for (vector<ChanEvent *>::const_iterator itA = pulserEvents.begin(); itA != pulserEvents.end(); itA++)
            if (loc == 0)
                plot(DD_TEMPLATE_VS_PULSER, (*it)->GetEnergy(), (*itA)->GetEnergy());
    } */
    return true;
}
