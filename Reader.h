#ifndef READER_H_
#define READER_H_

#include "Run.h"
#include "pugi/pugixml.hpp"
#include <string>

#ifdef OPTIMET_BELOS
#include <Teuchos_ParameterList.hpp>
#endif

namespace optimet {
//! Reads simulation configuration from input
Run simulation_input(std::string const &filename);
}

#endif /* READER_H_ */
