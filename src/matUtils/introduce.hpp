#include "common.hpp"
// #include "select.hpp"

po::variables_map parse_introduce_command(po::parsed_options parsed);
std::map<std::string, std::vector<std::string>> read_two_column (std::string sample_filename);
std::vector<std::string> find_introductions(MAT::Tree* T, std::vector<std::string> samples, std::vector<std::string> regions); 
std::map<std::string, int> get_assignments(MAT::Tree* T, std::unordered_set<std::string> sample_set);
void introduce_main(po::parsed_options parsed);