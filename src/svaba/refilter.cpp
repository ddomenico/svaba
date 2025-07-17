#include "refilter.h"
#include "DBSnpFilter.h"

#include <getopt.h>
#include <sstream>
#include <iostream>

#include "gzstream.h"
#include "SeqLib/BamReader.h"

#include "vcf.h"
#include "BreakPoint.h"
#include "svabaUtils.h"


static DBSnpFilter * dbsnp_filter;
static ogzstream os_allbps_r;

namespace opt {

  static std::string input_file;
  static std::string output_file;
  static std::string pon = "";
  static std::string analysis_id = "refilter";
  static bool read_tracking = false;
  static std::string indel_mask;

  static std::string normal_bam;
  static std::string tumor_bam;

  static std::string bam;

  static std::string dbsnp; // = "/xchip/gistic/Jeremiah/svabaFilters/dbsnp_138.b37_indel.vcf";

  static int verbose = 1;

  // indel probability cutoffs
  static double lod = 8; // LOD that variant is not ref
  static double lod_db = 6; // same, but at DB snp site (want lower bc we have prior)
  static double lod_somatic = 6; // LOD that normal is REF
  static double lod_somatic_db = 10; // same, but at DBSNP (want higher bc we have prior that its germline)
  static double scale_error = 1; 
  static bool pass_only = false; // only output PASS variants
}

enum { 
  OPT_LOD,
  OPT_LOD_DB,
  OPT_LOD_SOMATIC,
  OPT_LOD_SOMATIC_DB,
  OPT_READ_TRACK,
  OPT_SCALE_ERRORS,
  OPT_PASS_ONLY
};


static const char* shortopts = "hi:a:v:G:D:b:";
static const struct option longopts[] = {
  { "help",                    no_argument, NULL, 'h' },
  { "input-bps",               required_argument, NULL, 'i'},
  { "bam",                     required_argument, NULL, 'b'},
  { "case-bam",                required_argument, NULL, 't' },
  { "control-bam",             required_argument, NULL, 'n' },
  { "reference-genome",        required_argument, NULL, 'G'},
  { "analysis-id",             required_argument, NULL, 'a'},
  { "verbose",                 required_argument, NULL, 'v' },
  { "lod",                     required_argument, NULL, OPT_LOD },
  { "lod-dbsnp",               required_argument, NULL, OPT_LOD_DB },
  { "lod-somatic",             required_argument, NULL, OPT_LOD_SOMATIC },
  { "lod-somatic-dbsnp",       required_argument, NULL, OPT_LOD_SOMATIC_DB },
  { "scale-errors",            required_argument, NULL, OPT_SCALE_ERRORS },
  { "read-tracking",           no_argument, NULL, OPT_READ_TRACK },
  { "pass-only",               no_argument, NULL, OPT_PASS_ONLY },
  { "dbsnp-vcf",               required_argument, NULL, 'D' },
  { NULL, 0, NULL, 0 }
};

static const char *BP_USAGE_MESSAGE =
"Usage: svaba refilter [OPTION] -i bps.txt.gz -b <bam>\n\n"
"  Description: \n"
"\n"
"  General options\n"
"  -v, --verbose                        Select verbosity level (0-4). Default: 1 \n"
"  -h, --help                           Display this help and exit\n"
"  -a, --id-string                      String specifying the analysis ID to be used as part of ID common.\n"
"  Required input\n"
"  -i, --input-bps                      Original bps.txt.gz file\n"
"  -b, --bam                            BAM file used to grab header from\n"
"  Optional external database\n"
"  -D, --dbsnp-vcf                      DBsnp database (VCF) to compare indels against\n"
"  Variant filtering and classification\n"
  "      --lod                            LOD cutoff to classify indel as non-REF (tests AF=0 vs AF=MaxLikelihood(AF)) [8]\n"
"      --lod-dbsnp                      LOD cutoff to classify indel as non-REF (tests AF=0 vs AF=MaxLikelihood(AF)) at DBSnp indel site [5]\n"
"      --lod-somatic                    LOD cutoff to classify indel as somatic (tests AF=0 in normal vs AF=ML(0.5)) [2.5]\n"
"      --lod-somatic-dbsnp              LOD cutoff to classify indel as somatic (tests AF=0 in normal vs AF=ML(0.5)) at DBSnp indel site [4]\n"
"      --scale-errors                   Scale the priors that a site is artifact at given repeat count. 0 means assume low (const) error rate [1]\n"
"  Optional input\n"                       
"      --read-tracking                  Track supporting reads by qname. Increases file sizes. [off]\n"
"      --pass-only                      Only output PASS variants. Default: false\n"
"\n";

// parse the command line options
void parseBreakOptions(int argc, char** argv) {
  bool die = false;
  
  if (argc <= 2) 
    die = true;
  
  std::string tmp;
  for (char c; (c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1;) {
    std::istringstream arg(optarg != NULL ? optarg : "");
    switch (c) {
    case 'h': die = true; break;
    case 'i': arg >> opt::input_file; break;
    case 'v': arg >> opt::verbose; break;
    case 'a': arg >> opt::analysis_id; break;
    case 'D': arg >> opt::dbsnp; break;
    case OPT_LOD: arg >> opt::lod; break;
    case OPT_LOD_DB: arg >> opt::lod_db; break;
    case OPT_LOD_SOMATIC: arg >> opt::lod_somatic; break;
    case OPT_LOD_SOMATIC_DB: arg >> opt::lod_somatic_db; break;
    case OPT_READ_TRACK: opt::read_tracking = false; break;
    case OPT_SCALE_ERRORS: arg >> opt::scale_error; break;
    case OPT_PASS_ONLY: opt::pass_only = true; break;
    case 'b': arg >> opt::bam; break; 
    }
  }

  // just check if input here. Check later if readable
  if (opt::input_file.length() == 0)
    die = true;
  
  if (opt::bam.length() == 0) {
    std::cerr << "BAM is required (for the header)" << std::endl;
    die = true;
  }

  if (die) {
    std::cerr << "\n" << BP_USAGE_MESSAGE;
    exit(1);
  }
}

void runRefilterBreakpoints(int argc, char** argv) {
  
  parseBreakOptions(argc, argv);
  
  opt::output_file = opt::analysis_id + ".filtered.bps.txt.gz";
  if (opt::verbose > 0) {

    std::cerr << "Input bps file:  " << opt::input_file << std::endl;
    std::cerr << "Output bps file: " << opt::output_file << std::endl;
    std::cerr << "Panel of normals file: " << opt::pon << std::endl;
    std::cerr << "Indel mask BED:      " << opt::indel_mask << std::endl;
    std::cerr << "Analysis id: " << opt::analysis_id << std::endl << 
      "    LOD cutoff (non-REF):            " << opt::lod << std::endl << 
      "    LOD cutoff (non-REF, at DBSNP):  " << opt::lod_db << std::endl << 
      "    LOD somatic cutoff:              " << opt::lod_somatic << std::endl << 
      "    LOD somatic cutoff (at DBSNP):   " << opt::lod_somatic_db << std::endl << 
      "    DBSNP Database file: " << opt::dbsnp << std::endl;
  }

  if (!SeqLib::read_access_test(opt::input_file)) {
    std::cerr << "ERROR: Cannot read file " << opt::input_file  << std::endl;
    exit(EXIT_FAILURE);
  }
  
  SeqLib::BamReader bwalker;
  assert(bwalker.Open(opt::bam));
  
  // open the DBSnpFilter
  if (opt::dbsnp.length()) {
    std::cerr << "...loading the DBsnp database" << std::endl;
    dbsnp_filter = new DBSnpFilter(opt::dbsnp, bwalker.Header());
    std::cerr << "...loaded DBsnp database" << std::endl;
  }
  
  // start a new VCF file
  VCFHeader header;
  header.filedate = svabaUtils::fileDateString();
  header.source = "";//opt::args;
  header.reference = "";//opt::refgenome;
  
  // open bps file
  std::string new_bps_file = opt::analysis_id + ".bps.txt.gz";
  svabaUtils::fopen(new_bps_file, os_allbps_r);

  SeqLib::BamHeader hdr = bwalker.Header();  

    
  // read in the bps.txt.gz file
  std::vector<std::string> allele_names; // store with real name
  std::map<std::string, SampleInfo> tmp_alleles;
  igzstream infile(opt::input_file.c_str(), std::ios::in);
  size_t line_count = 0;

  // Read the header line first
  std::string headerLine;
  if (std::getline(infile, headerLine)) {
    std::vector<std::string> headerv = svabaUtils::tokenize_delimited(headerLine, '\t');

    // assume a certain format for bps.txt
    assert(headerv.size() >= 39);
    // everything at (0-based) 38 and above is a sample id
    for (size_t i = 38; i < headerv.size(); i++) {
      assert(headerv[i].at(0) == 't' || headerv[i].at(0) == 'n');
      allele_names.push_back(headerv[i]);
    }
  }
  
  // Now read the rest of the lines
  std::string line;
  while (std::getline(infile, line)) {
    
    if (line_count % 100000 == 0) 
      std::cerr << "...read " << opt::input_file << " at line " << SeqLib::AddCommas(line_count) << std::endl;
    
    BreakPoint * bp = new BreakPoint(line, hdr);
    
    // fill in with the correct names from the header of bps.txt
    std::string id ;
    for (auto& i : allele_names) {
      id += "A";
      tmp_alleles[i] = bp->allele[id];
    }
    bp->allele = tmp_alleles;
    
    // fill in discordant info
    for (auto& i : bp->allele) {
      if (i.first.at(0) == 't')
	bp->dc.tcount += i.second.disc;
      else
	bp->dc.ncount += i.second.disc;
    }
    
    // match against DBsnp database. Modify bp in place
    if (dbsnp_filter && opt::dbsnp.length()) 
      dbsnp_filter->queryBreakpoint(*bp);
    
    // score them
    bp->scoreBreakpoint(opt::lod, opt::lod_db, opt::lod_somatic, opt::lod_somatic_db, opt::scale_error, 0);
    os_allbps_r << bp->toFileString(!opt::read_tracking) << std::endl;
    delete bp;
    
    ++line_count;
  } // end the main bps read loop
  
  // close the bps read file
  os_allbps_r.close();
  
  // primary VCFs
  if (SeqLib::read_access_test(new_bps_file)) {
    
    if (opt::verbose)
      std::cerr << "...making the primary VCFs (unfiltered and filtered) from file " << new_bps_file << std::endl;
    
    VCFFile snowvcf(new_bps_file, opt::analysis_id, bwalker.Header(), header, !opt::pass_only,
		    opt::verbose > 0);
    
    std::string basename = opt::analysis_id + ".svaba.unfiltered.";
    snowvcf.include_nonpass = true;
    snowvcf.writeIndels(basename, false, allele_names.size() == 1);
    snowvcf.writeSVs(basename, false, allele_names.size() == 1);
    
    basename = opt::analysis_id + ".svaba.";
    snowvcf.include_nonpass = false;
    snowvcf.writeIndels(basename, false, allele_names.size() == 1);
    snowvcf.writeSVs(basename, false, allele_names.size() == 1);
    
  } else {
    std::cerr << "Failed to make VCF. Could not file bps file " << opt::input_file << std::endl;
  }
}
