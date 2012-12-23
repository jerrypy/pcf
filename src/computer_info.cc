/**
 * Copyright (C) 2012 Jakub Jirasek <xjiras02@stud.fit.vutbr.cz>
 *                    Libor Polčák <ipolcak@fit.vutbr.cz>
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

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

#include "clock_skew_guard.h"
#include "computations.h"
#include "packet_time_info.h"
#include "point2d.h"

const size_t STRLEN_MAX = 100;

double NaN = std::numeric_limits<double>().quiet_NaN();

void computer_info::insert_packet(double packet_delivered, uint32_t timestamp)
{
  packet_time_info new_packet;

  new_packet.time = packet_delivered;
  new_packet.timestamp = timestamp;

  if (freq != 0) {
    set_offset(new_packet, *(packets.begin()), freq);
  }

  packets.push_back(new_packet);
  last_packet_time = packet_delivered;

#ifdef PACKETS
  printf("Time: %.6lf\n", new_packet.time);
  printf("Timestamp: %lu\n\n", new_packet.timestamp);
#endif
}

void computer_info::insert_packet(double packet_delivered, uint32_t timestamp, clock_skew_guard &skews)
{
  insert_packet(packet_delivered, timestamp);

  if ((get_packets_count() % block_size) == 0) {
    block_finished(packet_delivered, skews);
  }
}



void computer_info::block_finished(double packet_delivered, clock_skew_guard &skews)
{
  // Set frequency
  if (freq == 0) {
    if ((packet_delivered - start_time) < 60) {
      return;
    }
    else {
      freq = compute_freq();
#if 0
      fprintf(stderr, "Found %s with frequency %d", address.c_str(), freq);
#endif

      const packet_time_info &first = *(packets.begin());
      if (freq != 0) {
        for (auto it = packets.begin(); it != packets.end(); ++it) {
          set_offset(*it, first, freq);
        }
      }
      else {
        return;
      }
    }
  }

  /// Save offsets into file
  save_packets(1);

  /// Set skew
  clock_skew_pair new_skew = compute_skew(packets.begin(), packets.end());
  if (new_skew.first == NaN) {
#ifdef DEBUG
    fprintf(stderr, "Clock skew not set for %s\n", address.c_str());
#endif
    return;
  }

  skew.alpha = new_skew.first;
  skew.beta = new_skew.second;

  skews.update_skew(address, skew.alpha);

  generate_graph(skews);

  /// Reduce packets
#ifdef REDUCE
  if (known_computer->count > (BLOCK * 5)) {
    reduce_packets(known_computer);
    /// Not enough packets removed -> remove packets from the beginning
    while (known_computer->count > (BLOCK * 3)) {
      for (int i = 0; i < BLOCK; i++) {
        known_computer->head_packet = remove_packet(known_computer->head_packet);
        known_computer->count--;
      }
    }
  }
#endif
}



void computer_info::restart(double packet_delivered, uint32_t timestamp)
{
  packets.clear();
  freq = 0;
  last_packet_time = packet_delivered;
  start_time = packet_delivered;
  skew.alpha = 0;
  skew.beta = 0;
  insert_packet(packet_delivered, timestamp);
}



clock_skew_pair computer_info::compute_skew(const packet_iterator &start, const packet_iterator &end)
{
  // Prepare an array of all points for convex hull computation
  unsigned long pckts_count = get_packets_count();
  point2d points[pckts_count];
  const packet_time_info &first = *(start);
  clock_skew_pair result(NaN, NaN);

  /// First point
  points[0].x = first.offset.x;
  points[0].y = first.offset.y;
  
  unsigned long i = 1;
  auto it = start;
  if (it == packets.end()) {
    return result;
  }
  else {
    ++it;
    if (it == packets.end()) {
      return result;
    }
  }
  for (; (it != end) && (it != packets.end()); ++it) {
    points[i].x = it->offset.x;
    points[i].y = it->offset.y;
    i++;
  }
  pckts_count = i-1;
  
  // Compute upper convex hull, note that points are destroyed inside the function
  // and pckts_count will refer to the number of points in the convex hull when
  // the function finish
  point2d *hull = convex_hull(points, &pckts_count);

  // alpha is tangent of the line, beta is the offset
  // y = alpha * x + beta
  double alpha, beta;

  // These two variables are used to compute the distance of a sector in the found convex hull to
  // all points. Min is the minimal distance, sum is valid for actual sector.
  double min, sum;

  // Compute j-th sector first
  unsigned long j = (pckts_count / 2);

  // Compute alpha, beta, min for the j-th sector (sum is not used atm)
  alpha = ((hull[j].y - hull[j - 1].y) / (hull[j].x - hull[j - 1].x));

  if (std::fabs(alpha) > 100) {
    return result;
  }

  beta = hull[j - 1].y - (alpha * hull[j - 1].x);
  min = 0.0;
  for (auto it = start; (it != end) && (it != packets.end()); ++it) {
    min += alpha * it->offset.x + beta - it->offset.y;
  }

  // Store computed alpha, beta; it may change if other sectors of convex hull are part
  // of the line with minimal distance
  result.first = alpha;
  result.second = beta;
  
#ifdef DEBUG
  printf("\n");
  printf("[%lf,%lf],[%lf,%lf], f(x) = %lf*x + %lf, sum = %lf\n", hull[j - 1].x, hull[j - 1].y, hull[j].x, hull[j].y, alpha, beta, min);
#endif

  // Compute alpha, beta, sum for other sectors
  for (i = 1; i < pckts_count; i++) {
    if (i == j) // We already computed j-th sector
      continue;
    
    alpha = ((hull[i].y - hull[i - 1].y) / (hull[i].x - hull[i - 1].x));
    
    /// Too steep
    if (alpha > 3 || alpha < -3)
      continue;
    
    beta = hull[i - 1].y - (alpha * hull[i - 1].x);
    
    /// SUM
    sum = 0.0;
    for (auto it = start; (it != end) && (it != packets.end()); ++it) {
      sum += (alpha * it->offset.x + beta - it->offset.y);
      if (sum >= min) // The sum is already higher than the sum of all points of other sectors
        break;
    }
    
#ifdef DEBUG
    printf("[%lf,%lf],[%lf,%lf], f(x) = %lf*x + %lf, sum = %lf\n", hull[i - 1].x, hull[i - 1].y, hull[i].x, hull[i].y, alpha, beta, sum);
#endif
    
    // A new min was fuound, update alpha, beta, and min
    if (sum < min) {
      result.first = alpha;
      result.second = beta;
      min = sum;
    }
  }
  
#ifdef DEBUG
  printf("f(x) = %lfx + %lf, min = %lf\n", result.first, result.second, min);
#endif
  
  return result;
}



int computer_info::compute_freq()
{
  assert(!packets.empty());

  if (get_packets_count() < 10) {
    // Wait for more packets
    return 0;
  }

  const packet_time_info &first = *packets.begin();

  double tmp = 0.0;
  int count = 0;

  for (auto it = ++packets.begin(); it != packets.end(); ++it) {
    double local_diff = it->time - first.time;
    if (local_diff > 60.0) {
      tmp += ((it->timestamp - first.timestamp) / local_diff);
      count++;
    }
  }

  /// According to the real world, but sometimes can be wrong, it depends...
  int freq = (int)round(tmp / count);
  if (freq >= 970 && freq <= 1030)
    freq = 1000;
  else if (freq >= 95 && freq <= 105)
    freq = 100;
  else if (freq >= 230 && freq <= 270)
    freq = 250;
  
#ifdef DEBUG
  printf("Frequency (Hz): %d\n", freq);
#endif
  
  return freq;
}



int computer_info::save_packets(short rewrite)
{
  FILE *f;
  char filename[50] = "log/";
  std::strcat(filename, get_address().c_str());
  std::strcat(filename, ".log");
#ifdef DEBUG
  int lines = 0;
#endif
  
  /// Open file
  if (rewrite == 1)
    f = fopen(filename, "w");
  else
    f = fopen(filename, "a");
  
  if (f == NULL) {
    fprintf(stderr, "Cannot save packets into the file: %s\n", filename);
    return(2);
  }

  /// Write to file
  char str[STRLEN_MAX];
  for (auto it = packets.begin(); it != packets.end(); ++it) {
    snprintf(str, STRLEN_MAX, "%lf\t%lf\n", it->offset.x, it->offset.y);
    fputs(str, f);
#ifdef DEBUG
    lines++;
#endif
  }
  
  /// Close file
  if (fclose(f) != 0)
    fprintf(stderr, "Cannot close file: %s\n", filename);

#ifdef DEBUG
    fprintf(stderr, "%s: %d lines written", get_address().c_str(), lines);
#endif
  
  return(0);
}



void time_to_str(char *buffer, size_t buffer_size, time_t time)
{
  struct tm time_data = *localtime(&time);
  strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", &time_data);
}



void computer_info::generate_graph(clock_skew_guard &skews)
{
  static const char* filename_template =  "graph/%s.gp";
  FILE *f;
  int filename_max = strlen(filename_template) + get_address().length();
  char filename[filename_max + 1];
  snprintf(filename, filename_max, filename_template, get_address().c_str());
  f = fopen(filename, "w");
  
  if (f == NULL) {
    fprintf(stderr, "Cannot create file: %s\n", filename);
    return;
  }
  
  if (get_skew().alpha == 0.0)
    return;

  const unsigned interval_count = 10;
  unsigned interval_min = packets.begin()->offset.x + get_start_time();
  unsigned interval_max = packets.rbegin()->offset.x + get_last_packet_time();
  unsigned interval_size = (interval_max - interval_min) / interval_count;

  fputs("set encoding iso_8859_2\n"
        "set terminal svg\n"
        "set output 'www/graph/", f);
        fputs(get_address().c_str(), f);
        fputs(".svg'\n\n"
        "set x2label 'Date and time'\n"
        "set xlabel 'Elapsed time [s]'\n"
        "set ylabel 'Offset [ms]'\n\n"
        "set x2tics axis in rotate by 270 textcolor lt 4\n"
        "set xtics mirror 0, ", f);
  fprintf(f, "%u\n\n", interval_size);
  fputs("set datafile separator '\\t'\n\n"
        "set x2tics ("
        , f);

  char tmp[100];

  time_t boundary = interval_min;
  unsigned boundary_m = 0;
  for (unsigned int i = 0; i <= interval_count; i++) {
    char date_time[STRLEN_MAX];
    time_to_str(date_time, STRLEN_MAX, boundary);
    sprintf(tmp, "'%s' %u", date_time, boundary_m);
    fputs(tmp, f);
    boundary += interval_size;
    boundary_m += interval_size;
    if (i < interval_count) {
      fputs(", ", f);
    }
  }

  fputs (")\n\nf(x) = ", f);
  /// f(x)
  sprintf(tmp, "%lf", get_skew().alpha);
  fputs(tmp, f);
  fputs("*x + ", f);
  sprintf(tmp, "%lf", get_skew().beta);
  fputs(tmp, f);
  fputs("\n\nset grid xtics x2tics ytics\n"
        "set title \"", f);
  
  /// Title
  time_t rawtime;
  time(&rawtime);
  sprintf(tmp, "%s", ctime(&rawtime));
  tmp[strlen(tmp)-1] = '\0';
  fputs(tmp, f);
  fputs("\\n", f);
  fputs(get_address().c_str(), f);

  // Search for computers with similar skew
  clock_skew_guard::address_containter similar_devices = skews.get_similar_identities(address);
  for (auto it = similar_devices.begin(); it != similar_devices.end(); ++it) {
    fputs("\\n", f);
    fputs(it->c_str(), f);
  }
  if (similar_devices.empty()) {
    fputs("\\nunknown\" textcolor lt 1", f);
    }
  else {
    fputs("\" textcolor lt 2", f);
  }

  /// Plot
  fputs("\n\n"
        "plot '", f);
  fputs("log/", f);
  fputs(get_address().c_str(), f);
  fputs(".log' title '', f(x)", f);
  
  /// Legend
  fputs(" title 'f(x) = ", f);
  sprintf(tmp, "%lf", get_skew().alpha);
  fputs(tmp, f);
  fputs("*x + ", f);
  sprintf(tmp, "%lf", get_skew().beta);
  fputs(tmp, f);
  fputs("'", f);
  
  if (fclose(f) != 0)
    fprintf(stderr, "Cannot close file: %s\n", filename);
  
  static const char* gnuplot_template =  "gnuplot graph/%s.gp";
  int gnuplot_max = strlen(gnuplot_template) + get_address().length();
  char gnuplot_cmd[gnuplot_max + 1];
  snprintf(gnuplot_cmd, gnuplot_max, gnuplot_template, get_address().c_str());
  system(gnuplot_cmd);
  
  return;
}
