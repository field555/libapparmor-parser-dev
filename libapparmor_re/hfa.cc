/*
 * (C) 2006, 2007 Andreas Gruenbacher <agruen@suse.de>
 * Copyright (c) 2003-2008 Novell, Inc. (All rights reserved)
 * Copyright 2009-2010 Canonical Ltd.
 *
 * The libapparmor library is licensed under the terms of the GNU
 * Lesser General Public License, version 2.1. Please see the file
 * COPYING.LGPL.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Base of implementation based on the Lexical Analysis chapter of:
 *   Alfred V. Aho, Ravi Sethi, Jeffrey D. Ullman:
 *   Compilers: Principles, Techniques, and Tools (The "Dragon Book"),
 *   Addison-Wesley, 1986.
 */

#include <list>
#include <vector>
#include <stack>
#include <set>
#include <map>
#include <ostream>
#include <iostream>
#include <fstream>

#include "expr-tree.h"
#include "hfa.h"
#include "../immunix.h"

ostream &operator<<(ostream &os, const State &state)
{
	/* dump the state label */
	os << '{';
	os << state.label;
	os << '}';
	return os;
}

State *DFA::add_new_state(NodeMap &nodemap,
			  pair<unsigned long, NodeSet *> index,
			  NodeSet *nodes, dfa_stats_t &stats)
{
	State *state = new State(nodemap.size(), nodes);
	states.push_back(state);
	nodemap.insert(make_pair(index, state));
	stats.proto_sum += nodes->size();
	if (nodes->size() > stats.proto_max)
		stats.proto_max = nodes->size();
	return state;
}

State *DFA::find_target_state(NodeMap &nodemap, list<State *> &work_queue,
			      NodeSet *nodes, dfa_stats_t &stats)
{
	State *target;

	pair<unsigned long, NodeSet *> index = make_pair(hash_NodeSet(nodes), nodes);

	map<pair<unsigned long, NodeSet *>, State *, deref_less_than>::iterator x = nodemap.find(index);

	if (x == nodemap.end()) {
		/* set of nodes isn't known so create new state, and nodes to
		 * state mapping
		 */
		target = add_new_state(nodemap, index, nodes, stats);
		work_queue.push_back(target);
	} else {
		/* set of nodes already has a mapping so free this one */
		stats.duplicates++;
		delete(nodes);
		target = x->second;
	}

	return target;
}

void DFA::update_state_transitions(NodeMap &nodemap, list<State *> &work_queue,
				   State *state, dfa_stats_t &stats)
{
	/* Compute possible transitions for state->nodes.  This is done by
	 * iterating over all the nodes in state->nodes and combining the
	 * transitions.
	 *
	 * The resultant transition set is a mapping of characters to
	 * sets of nodes.
	 */
	NodeCases cases;
	for (NodeSet::iterator i = state->nodes->begin(); i != state->nodes->end(); i++)
		(*i)->follow(cases);

	/* Now for each set of nodes in the computed transitions, make
	 * sure that there is a state that maps to it, and add the
	 * matching case to the state.
	 */

	/* check the default transition first */
	if (cases.otherwise)
		state->cases.otherwise = find_target_state(nodemap, work_queue,
							   cases.otherwise,
							   stats);;

	/* For each transition from *from, check if the set of nodes it
	 * transitions to already has been mapped to a state
	 */
	for (NodeCases::iterator j = cases.begin(); j != cases.end(); j++) {
		State *target;
		target = find_target_state(nodemap, work_queue, j->second, stats);

		/* Don't insert transition that the default transition
		 * already covers
		 */
		if (target != state->cases.otherwise)
			state->cases.cases[j->first] = target;
	}
}

/* WARNING: This routine can only be called from within DFA creation as
 * the nodes value is only valid during dfa construction.
 */
void DFA::dump_node_to_dfa(void)
{
	cerr << "Mapping of States to expr nodes\n"
		"  State  <=   Nodes\n"
		"-------------------\n";
	for (Partition::iterator i = states.begin(); i != states.end(); i++)
		cerr << "  " << (*i)->label << " <= " << *(*i)->nodes << "\n";
}

/**
 * Construct a DFA from a syntax tree.
 */
DFA::DFA(Node *root, dfaflags_t flags): root(root)
{
	dfa_stats_t stats = { 0, 0, 0 };
	int i = 0;

	if (flags & DFA_DUMP_PROGRESS)
		fprintf(stderr, "Creating dfa:\r");

	for (depth_first_traversal i(root); i; i++) {
		(*i)->compute_nullable();
		(*i)->compute_firstpos();
		(*i)->compute_lastpos();
	}

	if (flags & DFA_DUMP_PROGRESS)
		fprintf(stderr, "Creating dfa: followpos\r");
	for (depth_first_traversal i(root); i; i++) {
		(*i)->compute_followpos();
	}

	NodeMap nodemap;
	NodeSet *emptynode = new NodeSet;
	nonmatching = add_new_state(nodemap,
				    make_pair(hash_NodeSet(emptynode), emptynode),
				    emptynode, stats);

	NodeSet *first = new NodeSet(root->firstpos);
	start = add_new_state(nodemap, make_pair(hash_NodeSet(first), first),
			      first, stats);

	/* the work_queue contains the states that need to have their
	 * transitions computed.  This could be done with a recursive
	 * algorithm instead of a work_queue, but it would be slightly slower
	 * and consume more memory.
	 *
	 * TODO: currently the work_queue is treated in a breadth first
	 *       search manner.  Test using the work_queue in a depth first
	 *       manner, this may help reduce the number of entries on the
	 *       work_queue at any given time, thus reducing peak memory use.
	 */
	list<State *> work_queue;
	work_queue.push_back(start);

	while (!work_queue.empty()) {
		if (i % 1000 == 0 && (flags & DFA_DUMP_PROGRESS))
			fprintf(stderr, "\033[2KCreating dfa: queue %zd\tstates %zd\teliminated duplicates %d\r",
				work_queue.size(), states.size(),
				stats.duplicates);
		i++;

		State *from = work_queue.front();
		work_queue.pop_front();

		/* Update 'from's transitions, and if it transitions to any
		 * unknown State create it and add it to the work_queue
		 */
		update_state_transitions(nodemap, work_queue, from, stats);

	}  /* while (!work_queue.empty()) */

	/* cleanup Sets of nodes used computing the DFA as they are no longer
	 * needed.
	 */
	for (depth_first_traversal i(root); i; i++) {
		(*i)->firstpos.clear();
		(*i)->lastpos.clear();
		(*i)->followpos.clear();
	}

	if (flags & DFA_DUMP_NODE_TO_DFA)
		dump_node_to_dfa();

	for (NodeMap::iterator i = nodemap.begin(); i != nodemap.end(); i++)
		delete i->first.second;
	nodemap.clear();

	if (flags & (DFA_DUMP_STATS))
		fprintf(stderr, "\033[2KCreated dfa: states %zd,\teliminated duplicates %d,\tprotostate sets: longest %u, avg %u\n",
			states.size(), stats.duplicates, stats.proto_max,
			(unsigned int)(stats.proto_sum / states.size()));

}

DFA::~DFA()
{
	for (Partition::iterator i = states.begin(); i != states.end(); i++)
		delete *i;
}

void DFA::dump_uniq_perms(const char *s)
{
	set<pair<uint32_t, uint32_t> > uniq;
	for (Partition::iterator i = states.begin(); i != states.end(); i++)
		uniq.insert(make_pair((*i)->accept, (*i)->audit));

	cerr << "Unique Permission sets: " << s << " (" << uniq.size() << ")\n";
	cerr << "----------------------\n";
	for (set<pair<uint32_t, uint32_t> >::iterator i = uniq.begin();
	     i != uniq.end(); i++) {
		cerr << "  " << hex << i->first << " " << i->second << dec << "\n";
	}
}

/* Remove dead or unreachable states */
void DFA::remove_unreachable(dfaflags_t flags)
{
	set<State *> reachable;
	list<State *> work_queue;

	/* find the set of reachable states */
	reachable.insert(nonmatching);
	work_queue.push_back(start);
	while (!work_queue.empty()) {
		State *from = work_queue.front();
		work_queue.pop_front();
		reachable.insert(from);

		if (from->cases.otherwise &&
		    (reachable.find(from->cases.otherwise) == reachable.end()))
			work_queue.push_back(from->cases.otherwise);

		for (Cases::iterator j = from->cases.begin(); j != from->cases.end(); j++) {
			if (reachable.find(j->second) == reachable.end())
				work_queue.push_back(j->second);
		}
	}

	/* walk the set of states and remove any that aren't reachable */
	if (reachable.size() < states.size()) {
		int count = 0;
		Partition::iterator i;
		Partition::iterator next;
		for (i = states.begin(); i != states.end(); i = next) {
			next = i;
			next++;
			if (reachable.find(*i) == reachable.end()) {
				if (flags & DFA_DUMP_UNREACHABLE) {
					cerr << "unreachable: " << **i;
					if (*i == start)
						cerr << " <==";
					if ((*i)->accept) {
						cerr << " (0x" << hex 
						     << (*i)->accept << " " 
						     << (*i)->audit << dec
						     << ')';
					}
					cerr << "\n";
				}
				State *current = *i;
				states.erase(i);
				delete(current);
				count++;
			}
		}

		if (count && (flags & DFA_DUMP_STATS))
			cerr << "DFA: states " << states.size() << " removed "
			     << count << " unreachable states\n";
	}
}

/* test if two states have the same transitions under partition_map */
bool DFA::same_mappings(State *s1, State *s2)
{
	if (s1->cases.otherwise && s1->cases.otherwise != nonmatching) {
		if (!s2->cases.otherwise || s2->cases.otherwise == nonmatching)
			return false;
		Partition *p1 = s1->cases.otherwise->partition;
		Partition *p2 = s2->cases.otherwise->partition;
		if (p1 != p2)
			return false;
	} else if (s2->cases.otherwise && s2->cases.otherwise != nonmatching) {
		return false;
	}

	if (s1->cases.cases.size() != s2->cases.cases.size())
		return false;
	for (Cases::iterator j1 = s1->cases.begin(); j1 != s1->cases.end(); j1++) {
		Cases::iterator j2 = s2->cases.cases.find(j1->first);
		if (j2 == s2->cases.end())
			return false;
		Partition *p1 = j1->second->partition;
		Partition *p2 = j2->second->partition;
		if (p1 != p2)
			return false;
	}

	return true;
}

/* Do simple djb2 hashing against a States transition cases
 * this provides a rough initial guess at state equivalence as if a state
 * has a different number of transitions or has transitions on different
 * cases they will never be equivalent.
 * Note: this only hashes based off of the alphabet (not destination)
 * as different destinations could end up being equiv
 */
size_t DFA::hash_trans(State *s)
{
	unsigned long hash = 5381;

	for (Cases::iterator j = s->cases.begin(); j != s->cases.end(); j++) {
		hash = ((hash << 5) + hash) + j->first;
		State *k = j->second;
		hash = ((hash << 5) + hash) + k->cases.cases.size();
	}

	if (s->cases.otherwise && s->cases.otherwise != nonmatching) {
		hash = ((hash << 5) + hash) + 5381;
		State *k = s->cases.otherwise;
		hash = ((hash << 5) + hash) + k->cases.cases.size();
	}

	hash = (hash << 8) | s->cases.cases.size();
	return hash;
}

/* minimize the number of dfa states */
void DFA::minimize(dfaflags_t flags)
{
	map<pair<uint64_t, size_t>, Partition *> perm_map;
	list<Partition *> partitions;

	/* Set up the initial partitions
	 * minimium of - 1 non accepting, and 1 accepting
	 * if trans hashing is used the accepting and non-accepting partitions
	 * can be further split based on the number and type of transitions
	 * a state makes.
	 * If permission hashing is enabled the accepting partitions can
	 * be further divided by permissions.  This can result in not
	 * obtaining a truely minimized dfa but comes close, and can speedup
	 * minimization.
	 */
	int accept_count = 0;
	int final_accept = 0;
	for (Partition::iterator i = states.begin(); i != states.end(); i++) {
		uint64_t perm_hash = 0;
		if (flags & DFA_CONTROL_MINIMIZE_HASH_PERMS) {
			/* make every unique perm create a new partition */
			perm_hash = ((uint64_t) (*i)->audit) << 32 |
				    (uint64_t) (*i)->accept;
		} else if ((*i)->audit || (*i)->accept) {
			/* combine all perms together into a single parition */
			perm_hash = 1;
		} /* else not an accept state so 0 for perm_hash */
		size_t trans_hash = 0;
		if (flags & DFA_CONTROL_MINIMIZE_HASH_TRANS)
			trans_hash = hash_trans(*i);
		pair<uint64_t, size_t> group = make_pair(perm_hash, trans_hash);
		map<pair<uint64_t, size_t>, Partition *>::iterator p = perm_map.find(group);
		if (p == perm_map.end()) {
			Partition *part = new Partition();
			part->push_back(*i);
			perm_map.insert(make_pair(group, part));
			partitions.push_back(part);
			(*i)->partition = part;
			if (perm_hash)
				accept_count++;
		} else {
			(*i)->partition = p->second;
			p->second->push_back(*i);
		}

		if ((flags & DFA_DUMP_PROGRESS) && (partitions.size() % 1000 == 0))
			cerr << "\033[2KMinimize dfa: partitions "
			     << partitions.size() << "\tinit " << partitions.size()
			     << " (accept " << accept_count << ")\r";
	}

	/* perm_map is no longer needed so free the memory it is using.
	 * Don't remove - doing it manually here helps reduce peak memory usage.
	 */
	perm_map.clear();

	int init_count = partitions.size();
	if (flags & DFA_DUMP_PROGRESS)
		cerr << "\033[2KMinimize dfa: partitions " << partitions.size()
		     << "\tinit " << init_count << " (accept "
		     << accept_count << ")\r";

	/* Now do repartitioning until each partition contains the set of
	 * states that are the same.  This will happen when the partition
	 * splitting stables.  With a worse case of 1 state per partition
	 * ie. already minimized.
	 */
	Partition *new_part;
	int new_part_count;
	do {
		new_part_count = 0;
		for (list<Partition *>::iterator p = partitions.begin();
		     p != partitions.end(); p++) {
			new_part = NULL;
			State *rep = *((*p)->begin());
			Partition::iterator next;
			for (Partition::iterator s = ++(*p)->begin(); s != (*p)->end();) {
				if (same_mappings(rep, *s)) {
					++s;
					continue;
				}
				if (!new_part) {
					new_part = new Partition;
					list<Partition *>::iterator tmp = p;
					partitions.insert(++tmp, new_part);
					new_part_count++;
				}
				new_part->push_back(*s);
				s = (*p)->erase(s);
			}
			/* remapping partition_map for new_part entries
			 * Do not do this above as it messes up same_mappings
			 */
			if (new_part) {
				for (Partition::iterator m = new_part->begin();
				     m != new_part->end(); m++) {
					(*m)->partition = new_part;
				}
			}
			if ((flags & DFA_DUMP_PROGRESS) && (partitions.size() % 100 == 0))
				cerr << "\033[2KMinimize dfa: partitions "
				     << partitions.size() << "\tinit "
				     << init_count << " (accept "
				     << accept_count << ")\r";
		}
	} while (new_part_count);

	if (partitions.size() == states.size()) {
		if (flags & DFA_DUMP_STATS)
			cerr << "\033[2KDfa minimization no states removed: partitions "
			     << partitions.size() << "\tinit " << init_count
			     << " (accept " << accept_count << ")\n";

		goto out;
	}

	/* Remap the dfa so it uses the representative states
	 * Use the first state of a partition as the representative state
	 * At this point all states with in a partion have transitions
	 * to states within the same partitions, however this can slow
	 * down compressed dfa compression as there are more states,
	 */
	for (list<Partition *>::iterator p = partitions.begin();
	     p != partitions.end(); p++) {
		/* representative state for this partition */
		State *rep = *((*p)->begin());

		/* update representative state's transitions */
		if (rep->cases.otherwise) {
			Partition *partition = rep->cases.otherwise->partition;
			rep->cases.otherwise = *partition->begin();
		}
		for (Cases::iterator c = rep->cases.begin(); c != rep->cases.end(); c++) {
			Partition *partition = c->second->partition;
			c->second = *partition->begin();
		}

//if ((*p)->size() > 1)
//cerr << rep->label << ": ";
		/* clear the state label for all non representative states,
		 * and accumulate permissions */
		for (Partition::iterator i = ++(*p)->begin(); i != (*p)->end(); i++) {
//cerr << " " << (*i)->label;
			(*i)->label = -1;
			rep->accept |= (*i)->accept;
			rep->audit |= (*i)->audit;
		}
		if (rep->accept || rep->audit)
			final_accept++;
//if ((*p)->size() > 1)
//cerr << "\n";
	}
	if (flags & DFA_DUMP_STATS)
		cerr << "\033[2KMinimized dfa: final partitions "
		     << partitions.size() << " (accept " << final_accept
		     << ")" << "\tinit " << init_count << " (accept "
		     << accept_count << ")\n";

	/* make sure nonmatching and start state are up to date with the
	 * mappings */
	{
		Partition *partition = nonmatching->partition;
		if (*partition->begin() != nonmatching) {
			nonmatching = *partition->begin();
		}

		partition = start->partition;
		if (*partition->begin() != start) {
			start = *partition->begin();
		}
	}

	/* Now that the states have been remapped, remove all states
	 * that are not the representive states for their partition, they
	 * will have a label == -1
	 */
	for (Partition::iterator i = states.begin(); i != states.end();) {
		if ((*i)->label == -1) {
			State *s = *i;
			i = states.erase(i);
			delete(s);
		} else
			i++;
	}

out:
	/* Cleanup */
	while (!partitions.empty()) {
		Partition *p = partitions.front();
		partitions.pop_front();
		delete(p);
	}
}

/**
 * text-dump the DFA (for debugging).
 */
void DFA::dump(ostream & os)
{
	for (Partition::iterator i = states.begin(); i != states.end(); i++) {
		if (*i == start || (*i)->accept) {
			os << **i;
			if (*i == start)
				os << " <==";
			if ((*i)->accept) {
				os << " (0x" << hex << (*i)->accept << " "
				   << (*i)->audit << dec << ')';
			}
			os << "\n";
		}
	}
	os << "\n";

	for (Partition::iterator i = states.begin(); i != states.end(); i++) {
		if ((*i)->cases.otherwise)
			os << **i << " -> " << (*i)->cases.otherwise << "\n";
		for (Cases::iterator j = (*i)->cases.begin();
		     j != (*i)->cases.end(); j++) {
			os << **i << " -> " << j->second << ":  "
			   << j->first << "\n";
		}
	}
	os << "\n";
}

/**
 * Create a dot (graphviz) graph from the DFA (for debugging).
 */
void DFA::dump_dot_graph(ostream & os)
{
	os << "digraph \"dfa\" {" << "\n";

	for (Partition::iterator i = states.begin(); i != states.end(); i++) {
		if (*i == nonmatching)
			continue;

		os << "\t\"" << **i << "\" [" << "\n";
		if (*i == start) {
			os << "\t\tstyle=bold" << "\n";
		}
		uint32_t perms = (*i)->accept;
		if (perms) {
			os << "\t\tlabel=\"" << **i << "\\n("
			   << perms << ")\"" << "\n";
		}
		os << "\t]" << "\n";
	}
	for (Partition::iterator i = states.begin(); i != states.end(); i++) {
		Cases &cases = (*i)->cases;
		Chars excluded;

		for (Cases::iterator j = cases.begin(); j != cases.end(); j++) {
			if (j->second == nonmatching)
				excluded.insert(j->first);
			else {
				os << "\t\"" << **i << "\" -> \"" << *j->second
				   << "\" [" << "\n";
				os << "\t\tlabel=\"" << j->first << "\"\n";
				os << "\t]" << "\n";
			}
		}
		if (cases.otherwise && cases.otherwise != nonmatching) {
			os << "\t\"" << **i << "\" -> \"" << *cases.otherwise
			   << "\" [" << "\n";
			if (!excluded.empty()) {
				os << "\t\tlabel=\"[^";
				for (Chars::iterator i = excluded.begin();
				     i != excluded.end(); i++) {
					os << *i;
				}
				os << "]\"" << "\n";
			}
			os << "\t]" << "\n";
		}
	}
	os << '}' << "\n";
}

/**
 * Compute character equivalence classes in the DFA to save space in the
 * transition table.
 */
map<uchar, uchar> DFA::equivalence_classes(dfaflags_t flags)
{
	map<uchar, uchar> classes;
	uchar next_class = 1;

	for (Partition::iterator i = states.begin(); i != states.end(); i++) {
		Cases & cases = (*i)->cases;

		/* Group edges to the same next state together */
		map<const State *, Chars> node_sets;
		for (Cases::iterator j = cases.begin(); j != cases.end(); j++)
			node_sets[j->second].insert(j->first);

		for (map<const State *, Chars>::iterator j = node_sets.begin();
		     j != node_sets.end(); j++) {
			/* Group edges to the same next state together by class */
			map<uchar, Chars> node_classes;
			bool class_used = false;
			for (Chars::iterator k = j->second.begin();
			     k != j->second.end(); k++) {
				pair<map<uchar, uchar>::iterator, bool> x = classes.insert(make_pair(*k, next_class));
				if (x.second)
					class_used = true;
				pair<map<uchar, Chars>::iterator, bool> y = node_classes.insert(make_pair(x.first->second, Chars()));
				y.first->second.insert(*k);
			}
			if (class_used) {
				next_class++;
				class_used = false;
			}
			for (map<uchar, Chars>::iterator k = node_classes.begin();
			     k != node_classes.end(); k++) {
			  /**
			   * If any other characters are in the same class, move
			   * the characters in this class into their own new
			   * class
			   */
				map<uchar, uchar>::iterator l;
				for (l = classes.begin(); l != classes.end(); l++) {
					if (l->second == k->first &&
					    k->second.find(l->first) == k->second.end()) {
						class_used = true;
						break;
					}
				}
				if (class_used) {
					for (Chars::iterator l = k->second.begin();
					     l != k->second.end(); l++) {
						classes[*l] = next_class;
					}
					next_class++;
					class_used = false;
				}
			}
		}
	}

	if (flags & DFA_DUMP_EQUIV_STATS)
		fprintf(stderr, "Equiv class reduces to %d classes\n",
			next_class - 1);
	return classes;
}

/**
 * Text-dump the equivalence classes (for debugging).
 */
void dump_equivalence_classes(ostream &os, map<uchar, uchar> &eq)
{
	map<uchar, Chars> rev;

	for (map<uchar, uchar>::iterator i = eq.begin(); i != eq.end(); i++) {
		Chars &chars = rev.insert(make_pair(i->second, Chars())).first->second;
		chars.insert(i->first);
	}
	os << "(eq):" << "\n";
	for (map<uchar, Chars>::iterator i = rev.begin(); i != rev.end(); i++) {
		os << (int)i->first << ':';
		Chars &chars = i->second;
		for (Chars::iterator j = chars.begin(); j != chars.end(); j++) {
			os << ' ' << *j;
		}
		os << "\n";
	}
}

/**
 * Replace characters with classes (which are also represented as
 * characters) in the DFA transition table.
 */
void DFA::apply_equivalence_classes(map<uchar, uchar> &eq)
{
    /**
     * Note: We only transform the transition table; the nodes continue to
     * contain the original characters.
     */
	for (Partition::iterator i = states.begin(); i != states.end(); i++) {
		map<uchar, State *> tmp;
		tmp.swap((*i)->cases.cases);
		for (Cases::iterator j = tmp.begin(); j != tmp.end(); j++)
			(*i)->cases.cases.
			    insert(make_pair(eq[j->first], j->second));
	}
}

#if 0
typedef set <ImportantNode *>AcceptNodes;
map<ImportantNode *, AcceptNodes> dominance(DFA & dfa)
{
	map<ImportantNode *, AcceptNodes> is_dominated;

	for (States::iterator i = dfa.states.begin(); i != dfa.states.end(); i++) {
		AcceptNodes set1;
		for (State::iterator j = (*i)->begin(); j != (*i)->end(); j++) {
			if (AcceptNode * accept = dynamic_cast<AcceptNode *>(*j))
				set1.insert(accept);
		}
		for (AcceptNodes::iterator j = set1.begin(); j != set1.end(); j++) {
			pair<map<ImportantNode *, AcceptNodes>::iterator, bool> x = is_dominated.insert(make_pair(*j, set1));
			if (!x.second) {
				AcceptNodes & set2(x.first->second), set3;
				for (AcceptNodes::iterator l = set2.begin();
				     l != set2.end(); l++) {
					if (set1.find(*l) != set1.end())
						set3.insert(*l);
				}
				set3.swap(set2);
			}
		}
	}
	return is_dominated;
}
#endif

static inline int diff_qualifiers(uint32_t perm1, uint32_t perm2)
{
	return ((perm1 & AA_EXEC_TYPE) && (perm2 & AA_EXEC_TYPE) &&
		(perm1 & AA_EXEC_TYPE) != (perm2 & AA_EXEC_TYPE));
}

/**
 * Compute the permission flags that this state corresponds to. If we
 * have any exact matches, then they override the execute and safe
 * execute flags.
 */
uint32_t accept_perms(NodeSet *state, uint32_t *audit_ctl, int *error)
{
	uint32_t perms = 0, exact_match_perms = 0;
	uint32_t audit = 0, exact_audit = 0, quiet = 0, deny = 0;

	if (error)
		*error = 0;
	for (NodeSet::iterator i = state->begin(); i != state->end(); i++) {
		MatchFlag *match;
		if (!(match = dynamic_cast<MatchFlag *>(*i)))
			continue;
		if (dynamic_cast<ExactMatchFlag *>(match)) {
			/* exact match only ever happens with x */
			if (!is_merged_x_consistent(exact_match_perms,
						    match->flag) && error)
				*error = 1;;
			exact_match_perms |= match->flag;
			exact_audit |= match->audit;
		} else if (dynamic_cast<DenyMatchFlag *>(match)) {
			deny |= match->flag;
			quiet |= match->audit;
		} else {
			if (!is_merged_x_consistent(perms, match->flag)
			    && error)
				*error = 1;
			perms |= match->flag;
			audit |= match->audit;
		}
	}

//if (audit || quiet)
//fprintf(stderr, "perms: 0x%x, audit: 0x%x exact: 0x%x eaud: 0x%x deny: 0x%x quiet: 0x%x\n", perms, audit, exact_match_perms, exact_audit, deny, quiet);

	perms |= exact_match_perms & ~(AA_USER_EXEC_TYPE | AA_OTHER_EXEC_TYPE);

	if (exact_match_perms & AA_USER_EXEC_TYPE) {
		perms = (exact_match_perms & AA_USER_EXEC_TYPE) |
			(perms & ~AA_USER_EXEC_TYPE);
		audit = (exact_audit & AA_USER_EXEC_TYPE) |
			(audit & ~AA_USER_EXEC_TYPE);
	}
	if (exact_match_perms & AA_OTHER_EXEC_TYPE) {
		perms = (exact_match_perms & AA_OTHER_EXEC_TYPE) |
			(perms & ~AA_OTHER_EXEC_TYPE);
		audit = (exact_audit & AA_OTHER_EXEC_TYPE) |
			(audit & ~AA_OTHER_EXEC_TYPE);
	}
	if (perms & AA_USER_EXEC & deny)
		perms &= ~AA_USER_EXEC_TYPE;

	if (perms & AA_OTHER_EXEC & deny)
		perms &= ~AA_OTHER_EXEC_TYPE;

	perms &= ~deny;

	if (audit_ctl)
		*audit_ctl = PACK_AUDIT_CTL(audit, quiet & deny);

// if (perms & AA_ERROR_BIT) {
//     fprintf(stderr, "error bit 0x%x\n", perms);
//     exit(255);
//}

	//if (perms & AA_EXEC_BITS)
	//fprintf(stderr, "accept perm: 0x%x\n", perms);
	/*
	   if (perms & ~AA_VALID_PERMS)
	   yyerror(_("Internal error accumulated invalid perm 0x%llx\n"), perms);
	 */

//if (perms & AA_CHANGE_HAT)
//     fprintf(stderr, "change_hat 0x%x\n", perms);

	if (*error)
		fprintf(stderr, "profile has merged rule with conflicting x modifiers\n");

	return perms;
}