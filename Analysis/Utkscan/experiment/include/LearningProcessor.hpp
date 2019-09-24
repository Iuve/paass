/** \file TemplateProcessor.hpp
 * \brief A Template class to be used to build others.
 * \author S. V. Paulauskas
 * \date October 26, 2014
 */
#ifndef __LEARNINGPROCESSOR_HPP__
#define __LEARNINGPROCESSOR_HPP__

#include "EventProcessor.hpp"

//! A generic processor to be used as a template for others
class LearningProcessor : public EventProcessor {
public:
    /** Default Constructor */
    LearningProcessor();

    /** Constructor Accepting an argument */
    LearningProcessor(double threshold);

    /** Default Destructor */
    ~LearningProcessor() {};

    /** Declares the plots for the processor */
    virtual void DeclarePlots(void);

    /** Performs the preprocessing, which cannot depend on other processors
    * \param [in] event : the event to process
    * \return true if preprocessing was successful */
    virtual bool PreProcess(RawEvent &event);

    /** Performs the main processsing, which may depend on other processors
    * \param [in] event : the event to process
    * \return true if processing was successful */
    virtual bool Process(RawEvent &event);

    /** \return The processed Template events */
    std::vector<ChanEvent *> GetTemplateEvents(void) const {
        return (evts_);
    }

private:
    double threshold_; //!< a variable global to the class
    std::vector<ChanEvent *> evts_; //!< vector of events for people to get
};

#endif // __TEMPLATEPROCESSOR_HPP__
