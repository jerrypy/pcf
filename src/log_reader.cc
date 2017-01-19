/**
 * Copyright (C) 2013 Libor Polčák <ipolcak@fit.vutbr.cz>
 *                    Barbora Frankova <xfrank08@stud.fit.vutbr.cz>
 * 
 * This file is part of pcf - PC fingerprinter.
 *
 * Pcf is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Pcf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pcf. If not, see <http://www.gnu.org/licenses/>.
 */

#include <limits.h>
#include <cstring>
#include <climits>
#include <iostream>
#include <fstream>
#include <unistd.h>

#include "Configurator.h"
#include "ComputerInfoList.h"
#include "gnuplot_graph.h"

/**
 * Print help
 */
void print_help()
{
  printf("Usage: log_reader file\n\n"
         "  -h\t\tPrint this help\n"
         "  file Name of the file to be parsed\n"
         "Examples:\n"
         "  log_reader log/192.168.1.1\n\n");
}


/**
 * Read the content of the log file and process it
 */
void process_log_file(std::ifstream &ifs)
{
  
  ComputerInfoList * computers = new ComputerInfoList("tcp");
  gnuplot_graph graph_creator("tcp");
  computers->AddObserver(&graph_creator);
 
  double ttime, offset, arrival_time, timestamp;

  while (ifs.good()) {
    ifs >> ttime >> offset >> arrival_time >> timestamp;
    
    if (ifs.good()) {
      computers->new_packet("log_reader", 0, arrival_time, timestamp);
    }
  }
}

/**
 * Main
 */

int main(int argc, char *argv[])
{
  // Get config
  char filename[] = "config";
  Configurator::instance()->GetConfig(filename);
  
  int c;
  opterr = 0;
  while ((c = getopt(argc, argv, "hrdv")) != -1) {
    switch (c) {
      case('r'):
        Configurator::instance()->reduce = true;
        break;
      case('h'):
        print_help();
        return 0;
      case('d'):
        Configurator::instance()->portEnable = true;
        break;
      case('v'):
        Configurator::instance()->verbose = true;
        break;
    }
  }
  
  // Open log file
  std::ifstream ifs (argv[argc-1], std::ifstream::in);
  if (ifs.fail()) {
    std::cerr << "Failed to open file " << argv[1] << std::endl;
    return 2;
  }
  Configurator::instance()->timeLimit = INT_MAX;

  process_log_file(ifs);
}
