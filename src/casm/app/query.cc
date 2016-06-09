#include <string>
#include <boost/algorithm/string.hpp>
#include "casm/app/casm_functions.hh"
#include "casm/CASM_classes.hh"
#include "casm/clex/ConfigIO.hh"
#include "casm/clex/ConfigIOSelected.hh"
#include "casm/completer/Complete.hh"


namespace CASM {

  void query_help(const DataFormatterDictionary<Configuration> &_dict, std::ostream &_stream, std::vector<std::string > help_opt_vec) {
    _stream << "Prints the properties for a set of configurations for the set of currently selected" << std::endl
            << "configurations or for a set of configurations specifed by a selection file." << std::endl
            << std::endl
            << "Property values are output in column-separated (default) or JSON format.  By default, " << std::endl
            << "entries for 'name' and 'selected' values are included in the output. " << std::endl
            << std::endl;

    for(const std::string &help_opt : help_opt_vec) {
      if(help_opt.empty())
        continue;

      if(help_opt[0] == 'o') {
        _stream << "Available operators for use within queries:" << std::endl;
        _dict.print_help(_stream, BaseDatumFormatter<Configuration>::Operator);
      }
      else if(help_opt[0] == 'p') {
        _stream << "Available property tags are currently:" << std::endl;
        _dict.print_help(_stream, BaseDatumFormatter<Configuration>::Property);
      }
      _stream << std::endl;
    }
    _stream << std::endl;
  }

  namespace Completer {

    void QueryOption::initialize() {
      add_general_help_suboption();
      add_config_suboption();
      add_output_suboption();
      add_gzip_suboption();

      m_desc.add_options()
      ("columns,k", po::value<std::vector<std::string> >(&m_columns_vec)->multitoken()->zero_tokens()->value_name(ArgHandler::query()), "List of values you want printed as columns")
      ("json,j", po::value(&m_json_flag)->default_value(false)->zero_tokens(), "Print in JSON format (CSV otherwise, unless output extension is .json/.JSON)")
      ("verbatim,v", po::value(&m_verbatim_flag)->default_value(false)->zero_tokens(), "Print exact properties specified, without prepending 'name' and 'selected' entries")
      ("all,a", "Print results all configurations in input selection, whether or not they are selected.")
      ("no-header,n", po::value(&m_no_header_flag)->default_value(false)->zero_tokens(), "Print without header (CSV only)")
      ("alias", po::value<std::vector<std::string> >(&m_new_alias_vec)->multitoken(),
       "Create an alias for a query that will persist within this project. "
       "Ex: 'casm query --alias is_Ni_dilute = lt(atom_frac(Ni),0.10001)'");

      return;
    }

    const std::vector<std::string> &QueryOption::columns_vec() const {
      return m_columns_vec;
    }

    const std::vector<std::string> &QueryOption::new_alias_vec() const {
      return m_new_alias_vec;
    }

    bool QueryOption::json_flag() const {
      return m_json_flag;
    }

    bool QueryOption::verbatim_flag() const {
      return m_verbatim_flag;
    }

    bool QueryOption::no_header_flag() const {
      return m_no_header_flag;
    }

  }

  int query_command(const CommandArgs &args) {

    //std::string selection_str;
    //fs::path out_path;
    //std::vector<std::string> columns, help_opt_vec, new_alias;

    // Set command line options using boost program_options
    Completer::QueryOption query_opt("query");

    //You'll need to alter the values given in query, so make a copy of them after parsing
    bool json_flag, no_header_flag, verbatim_flag, gz_flag;

    fs::path config_path;
    po::variables_map vm;

    try {
      po::store(po::parse_command_line(args.argc, args.argv, query_opt.desc()), vm); // can throw

      /** Start --help option
       */
      if(vm.count("help")) {
        args.log << std::endl << query_opt.desc() << std::endl;
      }

      po::notify(vm); // throws on error, so do after help in case of problems

      //These values get reset throughout, so make a copy whatever was passed
      json_flag = query_opt.json_flag();
      no_header_flag = query_opt.no_header_flag();
      verbatim_flag = query_opt.verbatim_flag();
      gz_flag = query_opt.gzip_flag();

      /** Finish --help option
       */
      if(vm.count("help")) {
        if(args.root.empty()) {
          auto dict = make_dictionary<Configuration>();
          query_help(dict, std::cout, query_opt.help_opt_vec());
        }
        else {
          ProjectSettings set(args.root);
          query_help(set.config_io(), args.log, query_opt.help_opt_vec());
        }
        return 0;
      }


    }
    catch(po::error &e) {
      args.err_log << "ERROR: " << e.what() << std::endl << std::endl;
      args.err_log << query_opt.desc() << std::endl;
      return ERR_INVALID_ARG;
    }
    catch(std::exception &e) {
      args.err_log << "Unhandled Exception reached the top of main: "
                   << e.what() << ", application will now exit" << std::endl;
      return ERR_UNKNOWN;
    }

    if(!vm.count("alias") && !vm.count("columns")) {
      args.log << std::endl << query_opt.desc() << std::endl;
    }

    // set current path to project root
    const fs::path &root = args.root;
    if(root.empty()) {
      args.err_log.error("No casm project found");
      args.err_log << std::endl;
      return ERR_NO_PROJ;
    }

    if(vm.count("alias")) {

      ProjectSettings set(root);

      // get user input
      std::string new_alias_str;
      for(auto const &substr : query_opt.new_alias_vec()) {
        new_alias_str += substr;
      }

      // parse new_alias_str to create formatter
      auto it = std::find(new_alias_str.cbegin(), new_alias_str.cend(), '=');
      std::string alias_name = boost::trim_copy(std::string(new_alias_str.cbegin(), it));
      std::string alias_command = boost::trim_copy(std::string(++it, new_alias_str.cend()));

      try {
        set.add_alias(alias_name, alias_command, args.err_log);
        set.commit();
        return 0;
      }
      catch(std::runtime_error &e) {
        args.err_log << "Unable to learn alias\n"
                     << "   \"" << alias_name << " = " << alias_command << "\"\n"
                     << e.what() << std::endl;
        return ERR_UNKNOWN;
      }

    }
    if(!vm.count("columns")) {
      args.err_log << "ERROR: the option '--columns' is required but missing" << std::endl;
      return ERR_INVALID_ARG;
    }


    // -------------------------------------------------------------------------
    // perform query operation

    auto check_gz = [ = ](fs::path p) {
      if(p.extension() == ".gz" || p.extension() == ".GZ") {
        return true;
      }
      return false;
    };

    auto check_json = [ = ](fs::path p) {
      if(p.extension() == ".json" || p.extension() == ".JSON") {
        return true;
      }
      return false;
    };


    // Checks for: X.json.gz / X.json / X.gz  (also accepts .JSON or .GZ)
    if(check_gz(query_opt.output_path())) {
      gz_flag = true;
      json_flag = check_json(query_opt.output_path().stem()) || json_flag;
    }
    else {
      json_flag = check_json(query_opt.output_path()) || json_flag;
    }

    // set output_stream: where the query results are written
    std::unique_ptr<std::ostream> uniq_fout;
    std::ostream &output_stream = make_ostream_if(vm.count("output"), args.log, uniq_fout, query_opt.output_path(), gz_flag);
    output_stream << FormatFlag(output_stream).print_header(!no_header_flag);

    // If '_primclex', use that, else construct PrimClex in 'uniq_primclex'
    // Then whichever exists, store reference in 'primclex'
    std::unique_ptr<PrimClex> uniq_primclex;
    PrimClex &primclex = make_primclex_if_not(args, uniq_primclex);

    // Get configuration selection
    ConstConfigSelection selection(primclex, query_opt.selection_str());

    // set status_stream: where query settings and PrimClex initialization messages are sent
    Log &status_log = (query_opt.output_path().string() == "STDOUT") ? args.err_log : args.log;

    // Print info
    status_log << "Print:" << std::endl;
    for(int p = 0; p < query_opt.columns_vec().size(); p++) {
      status_log << "   - " << query_opt.columns_vec()[p] << std::endl;
    }
    if(vm.count("output"))
      status_log << "to " << fs::absolute(query_opt.output_path()) << std::endl;
    status_log << std::endl;

    // Construct DataFormatter
    primclex.settings().set_selected(selection);
    DataFormatter<Configuration> formatter;

    try {

      std::vector<std::string> all_columns;
      if(!verbatim_flag) {
        all_columns.push_back("configname");
        all_columns.push_back("selected");
      }
      all_columns.insert(all_columns.end(), query_opt.columns_vec().cbegin(), query_opt.columns_vec().cend());

      formatter.append(primclex.settings().config_io().parse(all_columns));

    }
    catch(std::exception &e) {
      args.err_log << "Parsing error: " << e.what() << "\n\n";
      return ERR_INVALID_ARG;
    }

    try {

      auto begin = vm.count("all") ? selection.config_begin() : selection.selected_config_begin();
      auto end = vm.count("all") ? selection.config_end() : selection.selected_config_end();

      // JSON output block
      if(json_flag) {
        jsonParser json;

        //sout << "Read in config selection... it is:\n" << selection;
        json = formatter(begin, end);

        output_stream << json;
      }
      // CSV output block
      else {
        //sout << "Read in config selection... it is:\n" << selection;
        output_stream << formatter(begin, end);
      }

    }
    catch(std::exception &e) {
      args.err_log << "Initialization error: " << e.what() << "\n\n";
      return ERR_UNKNOWN;
    }

    if(!uniq_fout) {
      status_log << "\n   -Output printed to terminal, since no output file specified-\n";
    }

    status_log << "  DONE." << std::endl << std::endl;

    return 0;
  };
}


