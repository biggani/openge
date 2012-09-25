/*********************************************************************
 *
 * bpipe.cpp: A bpipe script object
 * Open Genomics Engine
 *
 * Author: Lee C. Baker, VBI
 * Last modified: 12 September 2012
 *
 *********************************************************************
 *
 * This file is released under the Virginia Tech Non-Commercial
 * Purpose License. A copy of this license has been provided in
 * the openge/ directory.
 *
 *********************************************************************/

#include "bpipe.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <iterator>
#include <map>
#include <ctime>

#include <boost/spirit/include/qi.hpp>

#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_stl.hpp>
#include <boost/spirit/include/phoenix_bind.hpp>
#include <boost/spirit/include/phoenix.hpp>
#include <boost/spirit/home/phoenix/object/new.hpp>
#include <boost/spirit/home/phoenix/container.hpp>
using namespace std;

namespace qi = boost::spirit::qi;
namespace ascii = boost::spirit::ascii;
namespace phoenix = boost::phoenix;

struct stage {
    string name;
    vector<string> exec_lines;
    string filter;
    bool forward_input;

    void setName(const string & name) { this->name = name; }
    
    stage(const stage & s)
    : name(s.name)
    , exec_lines(s.exec_lines)
    {}
    stage() {}
};

typedef map<string,string> variable_storage_t;

class StageQueue {
public:
    virtual bool check(variable_storage_t & variables) { return true; }
    virtual bool execute() = 0;
    virtual void print() { cerr << "(no description)"; }
};

class ParallelStageQueue : public StageQueue {
    StageQueue * q1, * q2;
public:
    ParallelStageQueue(StageQueue * q1, StageQueue * q2)
    : q1(q1)
    , q2(q2)
    {}
    
    virtual bool check(variable_storage_t & variables) { return q1->check(variables) && q2->check(variables); }
    virtual bool execute() { return q1->execute() && q2->execute(); }
    virtual void print() { cerr << "Parallel(";q1->print();cerr << ","; q2->print(); cerr << ")"; }
};

class SerialStageQueue : public StageQueue {
    StageQueue * q1, * q2;
public:
    SerialStageQueue(StageQueue * q1, StageQueue * q2)
    : q1(q1)
    , q2(q2)
    {}
    virtual bool check(variable_storage_t & variables) { return q1->check(variables) && q2->check(variables); }
    virtual bool execute() { return q1->execute() && q2->execute(); }
    virtual void print() { cerr << "Serial(";q1->print();cerr << ","; q2->print(); cerr << ")"; }
};

class StageReference : public StageQueue {
    string name, filter;
    vector<stage> & stages;
    vector<string> commands;
    stage * s;
public:
    StageReference(string name, vector<stage> & stages)
    : name(name)
    , stages(stages)
    , s(NULL)
    {}
    
    virtual bool check(variable_storage_t & variables);
    virtual bool execute();
    virtual void print() { cerr << name; }
};

bool is_space(const int c) {
    return std::isspace(c) || c == 0;
}

bool is_var_name_char(const int c) {
    return isalnum(c) || c == '_';
}

bool is_not_var_name_char(const int c) {
    return ! is_var_name_char(c);
}

bool StageReference::check(variable_storage_t & variables) {
    for(vector<stage>::iterator si = stages.begin(); si != stages.end(); si++)
        if(si->name == name) {
            s = &*si;
            break;
        }
    
    if(!s) {
        cerr << "BPipe file error: stage name '" << name << "' didn't match any known stages." << endl;
        return false;
    }
    
    for(vector<string>::const_iterator e = s->exec_lines.begin(); e != s->exec_lines.end(); e++) {
        string command = *e;
        
        // replace $input and $output
        if(variables.count("input") != 0) {
            variables["output"] = variables["input"] + "." + name;
        }

        //find variables and substitute in values
        //variables may be in the form $VAR or ${VAR}, so we must handle both
        vector<string> variable_name;
        while(true) {
            string::iterator dollar = find(command.begin(), command.end(), '$');
            if(dollar == command.end())
                break;
            string::iterator var_end;
            string var_name;

            if(*(dollar + 1) != '{') {
                var_end = find_if(dollar+1, command.end(), is_not_var_name_char);
                var_name = string(dollar + 1, var_end);
            } else {
                var_end = find(dollar+2, command.end(), '}') + 1;
                var_name = string(dollar + 2, var_end-1);
            }

            if(variables.count(var_name) == 0) {
                cerr << "Variable " << var_name << " is not defined in stage " << name << endl;
                return false;
            } else {
                const string & val = variables[var_name];
                command.replace(dollar, var_end, val.begin(), val.end());
            }
        }
        
        commands.push_back(command);
    }

    //TODO if (not forward input)
    if(variables.count("output") != 0 && !s->forward_input)
        variables["input"] = variables["output"];

    return true;
}

bool StageReference::execute() {
    
    time_t now = time(NULL);
    char * time_str = ctime(&now);
    time_str[24] = 0;
    cerr << "=== Stage " << name << " " << time_str << " ===" << endl;
    int ret = 0;
    for(vector<string>::const_iterator i = commands.begin(); i != commands.end() && ret == 0; i++)
        ret = system(i->c_str());
    
    if(0 != ret)
        cerr << "Execution of stage failed (" << ret << ")." << endl;
    
    return 0 == ret;
}

template <typename Iterator>
struct BpipeParser : qi::grammar<Iterator, vector<stage>(), ascii::space_type>
{
    vector<stage> stages;
    map<string, stage> stage_names;
    map<string,string> global_vars;
    StageQueue * run_task;
    
    static stage & setStageName(string name, stage & s) {
        s.name = name;
        return s;
    }
    static stage & setFilter(string name, stage & s) {
        s.filter = name;
        return s;
    }
    
    static stage & addExecLine(string exec, stage & s) {
        s.exec_lines.push_back(exec);
        return s;
    }
    
    static stage & setForwardInput(stage & s) {
        s.forward_input = true;
        return s;
    }
    
    BpipeParser() : BpipeParser::base_type(start)
    {
        using qi::lit;
        using qi::lexeme;
        using ascii::char_;
        using qi::_val;
        using qi::_1;
        using qi::_2;
        using qi::space;
        using qi::alnum;
        using phoenix::ref;
        using phoenix::bind;
        using phoenix::construct;
        using phoenix::val;
        using phoenix::push_back;
        using phoenix::new_;
        using phoenix::insert;
        
        run_task = NULL;

        quoted_string.name("quoted_string");
        unquoted_string.name("unquoted_string");
        stage_block.name("stage_block");
        run_block.name("run_block");
        bpipe_file.name("bpipe_file");

        quoted_string = lexeme['"' >> +(char_ - '"') >> '"'];
        unquoted_string %= +(lit("\\\"")[_val='"'] | lit("\\\\")[_val='\\'] | alnum);
        
        doc_attribute_name = lit("title") | lit("author") | lit("constraints") | lit("desc");
        doc_statement = lit("doc") >> (quoted_string | *(doc_attribute_name >> lit(":") >> quoted_string >> -lit(",")));

        exec_statement = lit("exec") >> quoted_string >> -lit(";");
        msg_statement = lit("msg") >> quoted_string >> -lit(";");
        stage_block = (lit('{')[_val = construct<stage>()]) >> +(doc_statement | msg_statement | exec_statement[_val = bind(&addExecLine, _1, _val)]) >> -(lit("forward") >> lit("input") >> -lit(";"))[_val = bind(&setForwardInput,_val)] >> '}' ;
        stage_filter = (("{" >> lit("filter(") >> quoted_string >> lit(")") >> stage_generator >> "}")[_val = bind(&setFilter, _1, _2)] | ("@Filter(" >> quoted_string >> lit(")") >> stage_generator) [_val = bind(&setFilter, _1, _2)]);
        stage_assignment = (unquoted_string >> lit("=") >> stage_generator)[_val = bind(&setStageName, _1, _2)];
        stage_generator %=  stage_block | stage_assignment;
        stage_definition = stage_generator[push_back(ref(stages), _1)];
        var_assignment %= (unquoted_string >> lit("=") >> quoted_string)[insert(ref(global_vars),construct<pair<string,string> >(_1,_2))];
        
        stage_reference.name("StageReference");
        stage_serial_queue.name("StageSerialQueue");
        stage_parallel_queue.name("StageParallelQueue");
        stage_queue.name("StageQueue");
        run_block.name("RunBlock");

        stage_reference = unquoted_string [_val = new_<StageReference>(_1, ref(stages))];
        stage_serial_queue = (stage_parallel_queue | stage_reference)[_val = _1] >> *(('+' >> (stage_parallel_queue | stage_reference))[_val = new_<SerialStageQueue>(_val, _1)]);
        stage_parallel_queue = '[' >> stage_queue[_val = _1] >> *((',' >> stage_queue)[_val = new_<ParallelStageQueue>(_val, _1)]) >> ']';
        stage_queue %= (stage_parallel_queue | stage_serial_queue)[_val = _1];
        run_block = ((lit("Bpipe.run") | lit("run")) >> '{' >> stage_serial_queue >> '}')[ref(run_task) = _1] ;
        about_block = lit("about") >> lit("title") >> lit(":") >> quoted_string;
        bpipe_file %= *((stage_definition | var_assignment | about_block) >> -lit(";")) >> run_block;
        start %= bpipe_file;
    }

    //stage definitions
    qi::rule<Iterator, string(), ascii::space_type> quoted_string;
    qi::rule<Iterator, string(), ascii::space_type> unquoted_string;
    
    qi::rule<Iterator, ascii::space_type> doc_attribute_name;
    qi::rule<Iterator, ascii::space_type> doc_statement;
    qi::rule<Iterator, string(), ascii::space_type> exec_statement;
    qi::rule<Iterator, string(), ascii::space_type> msg_statement;
    qi::rule<Iterator, stage(), ascii::space_type> stage_generator;
    qi::rule<Iterator, stage(), ascii::space_type> stage_filter;
    qi::rule<Iterator, ascii::space_type> stage_definition;
    qi::rule<Iterator, stage(), ascii::space_type> stage_block;
    qi::rule<Iterator, stage(), ascii::space_type> stage_assignment;
    
    //run
    qi::rule<Iterator, ascii::space_type> run_block;
    qi::rule<Iterator, ascii::space_type> about_block;
    qi::rule<Iterator, ascii::space_type> var_assignment;
    qi::rule<Iterator, StageQueue*(), ascii::space_type> stage_reference;
    qi::rule<Iterator, StageQueue*(), ascii::space_type> stage_serial_queue;
    qi::rule<Iterator, StageQueue*(), ascii::space_type> stage_parallel_queue;
    qi::rule<Iterator, StageQueue*(), ascii::space_type> stage_queue;
    
    //file
    qi::rule<Iterator, vector<stage>(), ascii::space_type> bpipe_file;
    qi::rule<Iterator, vector<stage>(), ascii::space_type> start;
    
};

BPipe::BPipe() {
    
}

bool BPipe::load(const string & filename) {
    this->filename = filename;
    
    ifstream file(filename.c_str());
    
    if(file.fail()) {
        cerr << "Error opening file " << filename << ". Aborting." << endl;
        exit(-1);
    }
    
    //read in script
    string line;
    while(getline(file, line))
        script_text += line + "\n";

    file.close();
    
    
    //remove  /**/ comments
    while(true) {
        size_t cstart = script_text.find("/*");
        size_t cend = script_text.find("*/");
        
        if(cstart != string::npos && cend != string::npos)
            script_text.erase(cstart, cend - cstart +2);
        else
            break;
    }
    
    // remove // comments
    while(true) {
        size_t cstart = script_text.find("//");
        size_t cend = script_text.find("\n", cstart);
        
        if(cstart != string::npos && cend != string::npos)
            script_text.erase(cstart, cend - cstart);
        else
            break;
    }
    
    return true;
}

bool BPipe::check(const string & input_filename) {
    using qi::double_;
    using qi::phrase_parse;
    using ascii::space;
    
    string::const_iterator first = script_text.begin();
    string::const_iterator last = script_text.end();
    parser = new BpipeParser<string::const_iterator>();
    vector<stage> result;
    bool r = phrase_parse( first, last, *parser, space, result);
    
    string::const_iterator p = first;
    string::const_iterator q = script_text.begin();
    int d = distance(q, p);
    if (first != last) { // fail if we did not get a full match
        cerr << "Parsed up to " << string(script_text, d) << endl;
        return false;
    }
    else {
        variable_storage_t vars;
        if(!input_filename.empty())
            vars["input"] = input_filename;
        vars.insert(parser->global_vars.begin(), parser->global_vars.end());

        return parser->run_task->check(vars);
    }
    return false;
}

void BPipe::print() {
    
    parser->run_task->print();
}

bool BPipe::execute() {
    time_t start_time = time(NULL);
    char * start_time_str = ctime(&start_time);
    start_time_str[24] = 0;
    cerr << "=== Starting pipeline at " << start_time_str << " ===" << endl;
    bool ret = parser->run_task->execute();
    
    time_t stop_time = time(NULL);
    char * stop_time_str = ctime(&stop_time);
    stop_time_str[24] = 0;
    
    if(!ret)
        cerr << "=== Pipeline FAILED at " << stop_time_str << " ===" << endl;
    else {
        cerr << "=== Finished successfully at " << stop_time_str << " ===" << endl;
    }
    return ret;
}