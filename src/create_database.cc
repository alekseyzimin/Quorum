/* Quorum
 * Copyright (C) 2012  Genome group at University of Maryland.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <string>
#include <vector>

#include <jellyfish/atomic_bits_array.hpp>
#include <jellyfish/mer_dna.hpp>
#include <jellyfish/file_header.hpp>
#include <jellyfish/err.hpp>
#include <jellyfish/thread_exec.hpp>
#include <jellyfish/stream_manager.hpp>
#include <jellyfish/whole_sequence_parser.hpp>
#include <jellyfish/large_hash_array.hpp>
#include <src/create_database_cmdline.hpp>

static create_database_cmdline args;

using jellyfish::mer_dna;
typedef jellyfish::large_hash::array<mer_dna> mer_array;
typedef jellyfish::atomic_bits_array<uint64_t> val_array;
typedef std::vector<const char*> file_vector;
typedef jellyfish::stream_manager<file_vector::const_iterator> stream_manager;
typedef jellyfish::whole_sequence_parser<stream_manager> read_parser;

class database_header : public jellyfish::file_header {
public:
  database_header() : jellyfish::file_header() { }
  database_header(std::istream& is) : jellyfish::file_header(is) { }

  void bits(uint32_t b) { root_["bits"] = (Json::UInt)b; }
  uint32_t bits() const { return root_["bits"] .asUInt(); }
};

class hash_with_quality {
  mer_array      keys_;
  val_array      vals_;
  const uint64_t max_val_;

public:
  hash_with_quality(size_t size, uint16_t key_len, int bits, uint16_t reprobe_limit = 126) :
    keys_(size, key_len, 0, reprobe_limit),
    vals_(bits + 1, keys_.size()),
    max_val_((uint64_t)-1 >> (sizeof(uint64_t) * 8 - bits))
  { }

  bool add(const mer_dna& key, unsigned int quality) {
    bool is_new;
    size_t id;
    if(!keys_.set(key, &is_new, &id))
      return false;

    auto     entry = vals_[id];
    uint64_t oval;
    uint64_t nval = entry.get();;
    do {
      oval = nval;
      if((oval & 1) > quality)
        nval = 3;
      else if((oval >> 1) == max_val_)
        return true;
      else
        nval = oval + 2;
    } while(!entry.set(nval));
    return true;
  }

  void write(std::ostream& os) const {
    vals_.write(os);
    keys_.write(os);
  }

  mer_array& keys() { return keys_; }
  val_array& vals() { return vals_; }
};

class quality_mer_counter : public jellyfish::thread_exec {
  hash_with_quality& ary_;
  read_parser        parser_;
  const char         qual_thresh_;

public:
  quality_mer_counter(int nb_threads, hash_with_quality& ary, stream_manager& streams, char qual_thresh) :
    ary_(ary),
    parser_(4 * nb_threads, 100, 1, streams),
    qual_thresh_(qual_thresh)
  { }

  virtual void start(int thid) {
    mer_dna m, rm;

    while(true) {
      read_parser::job job(parser_);
      if(job.is_empty()) break;

      for(size_t i = 0; i < job->nb_filled; ++i) { // Process each read
        std::string& seq = job->data[i].seq;
        std::string& quals = job->data[i].qual;

        auto         base     = seq.begin();
        auto         qual     = quals.begin();
        unsigned int low_len  = 0; // Length of low quality stretch
        unsigned int high_len = 0; // Length of high quality stretch
        for( ; base != seq.begin(); ++base, ++base) {
          int code = mer_dna::code(*base);
          if(mer_dna::not_dna(code)) {
            high_len = low_len = 0;
            continue;
          }
          m.shift_left(code);
          m.shift_right(mer_dna::rev_code(code));
          ++low_len;
          if(*qual > qual_thresh_)
            ++high_len;
          else
            high_len = 0;
          if(low_len >= mer_dna::k()) {
            ary_.add(m < rm ? m : rm, high_len >= mer_dna::k());
          }
        }
      }
    }
  }
};

int main(int argc, char *argv[])
{
  database_header header;
  header.fill_standard();
  header.set_cmdline(argc, argv);

  args.parse(argc, argv);
  mer_dna::k(args.mer_arg);
  if(!args.min_qual_value_given && !args.min_qual_char_given)
    args.error("Either a min-qual-value or min-qual-char must be provided.");
  if(args.min_qual_char_arg.size() != 1)
    args.error("The min-qual-char should be one ASCII character.");
  char qual_thresh = args.min_qual_char_given ? args.min_qual_char_arg[0] : (char)args.min_qual_value_arg;
  if(args.bits_arg < 1 || args.bits_arg > 63)
    args.error("The number of bits should be between 1 and 63");
  std::ofstream output(args.output_arg);
  if(!output.good())
    die << "Failed to open output file '" << args.output_arg << "'.";

  hash_with_quality ary(args.size_arg, 2 * mer_dna::k(), args.bits_arg, args.reprobe_arg);
  {
    stream_manager streams(args.reads_arg.cbegin(), args.reads_arg.cend(), 1);
    quality_mer_counter counter(args.threads_arg, ary, streams, qual_thresh);
    counter.exec_join(args.threads_arg);
  }

  header.update_from_ary(ary.keys());
  header.bits(args.bits_arg);
  header.write(output);
  ary.write(output);
  output.close();

  return 0;
}
