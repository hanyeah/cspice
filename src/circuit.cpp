#include <algorithm>
#include <iomanip>
#include <iostream>
#include "element.h"
#include "circuit.h"
#include "utils.h"

#ifdef __PARALLEL__
#include <queue>
#include <pthread.h>

#ifndef threadcnt
#define threadcnt 6 // default threadcnt is 6
#endif

pthread_mutex_t __queMutex;
pthread_mutex_t __treeMutex;
pthread_mutex_t __idleMutex;
queue<PrimState> __taskQue;

int __idleNum,__busyNum;

#endif // __PARALLEL__

using namespace std ;
#ifdef __ELIMINATION__
template<class T>
static void eliminate(int index , vector< T >& array) {
   typedef typename std::vector<T>::iterator It;
   for (It it = array.begin() ; it != array.end() ; it ++) {
      if (index == 0) {
         array.erase(it) ; break ;
      } else {
         -- index ;
      }
   }
   return ;
}
#endif // __ELIMINATION__

void Circuit::propagateEquivalents(unsigned index , vector<bool>& visited) {
   const vector<Equivalent>& eq = this->nodes[index]->equivalents ;
   for(vector<Equivalent>::const_iterator it = eq.begin() ;
         it != eq.end() ; ++ it) {
      if (it->reason != this->currentSource) { // this source is not "on" currently
         unsigned nodeIndex = this->getIndexById(it->node->nodeId) ;
         if (visited[nodeIndex] != visited[index]) {
            // found a node with different status
            visited[nodeIndex] = visited[index] ;
            propagateEquivalents(nodeIndex , visited) ;
         }
      }
   }
}

#ifdef __ELIMINATION__
inline void elimSub(vector<const Element*> current_tree,
                    vector<vector<const Element*> >& result,
                    vector<pair<char , unsigned long long> >& trees) {
   char sign = 1;
   int eliminateId ;
   int amountOfElements = current_tree.size() ;
   int amountOfTrees ;
   unsigned long long hashValue = 0ull ;

   string concatName ;

   for (int i = 0 ; i < amountOfElements ; ++ i) {
      // if two elements only different in sign,
      // they should have same formula.
      // Ex: gm and -gm

      // use + , since it's commutative, that is,
      // the order of elements doesn't matter.

      // Be ware that, we have to take the risk of
      // hash(a) + hash(b) == hash(c) + hash(d),
      // though I don't think it would happen so easily.
      hashValue += hash(current_tree[i]->formula().c_str()) ;

      sign *= current_tree[i]->sign() ;
   }

   eliminateId = -1 ;
   amountOfTrees = trees.size() ;
   for (int i = 0 ; i < amountOfTrees ; ++ i) {
      if (trees[i].first == -sign && trees[i].second == hashValue) {
         eliminateId = i ; break ;
      }
   }

   if (eliminateId == -1) {
      result.push_back(current_tree) ;
      trees.push_back(pair<char , unsigned long long>(sign , hashValue)) ;
   } else {
      eliminate(eliminateId , result);
      eliminate(eliminateId , trees) ;
   }
}
#endif // __ELIMINATION__

#ifdef __PARALLEL__
void* processTask(void *arg) {
   /* task: shrink some edge to derive next PrimState */
   int v;
   PrimState ps = *((PrimState*)arg);

   for(v=0; v<ps.size; v++)
      if(!ps.visited[v]) break;
   if(v==ps.size) {
      //printf("add tree!\n");
      // add spanning tree to result, lock is needed
      pthread_mutex_lock(&__treeMutex);
      #ifdef __ELIMINATION__
      elimSub(ps.current_tree,*ps.result,*ps.trees);
      #else
      ps.result->push_back(ps.current_tree) ;
      #endif
      pthread_mutex_unlock(&__treeMutex);
      return NULL;
   }

   for(v=0; v<ps.size; v++) {
      if(ps.visited[v]) continue;
      vector<Connection>& adj = ps.circuit->nodes[v]->connections;
      int amountOfConnections = adj.size();
      for(int i=0 ; i<amountOfConnections ; i++) {
         unsigned desId = adj[i].destination->nodeId;
         unsigned u = ps.circuit->getIndexById(desId);
         if(!ps.visited[u]) continue;
         if(ps.used[v][i]) continue;
         ps.used[v][i]=1;
         PrimState ns = ps.shrink(v,adj[i].element);
         // push new state in queue, lock is needed
         pthread_mutex_lock(&__queMutex);
         __taskQue.push(ns);
         pthread_mutex_unlock(&__queMutex);
         //ps.startFrom[v] = amountOfConnections;
      }
   }

   return NULL;
}

void* dfsAdapter(void* arg) {
   PrimState ps = *((PrimState*)arg);

   pthread_mutex_lock(&__idleMutex);
   __busyNum++;
   __idleNum--;
   pthread_mutex_unlock(&__idleMutex);

   ps.circuit->dfs(ps.size,ps.visited,ps.used,ps.current_tree,*(ps.result)
                   #ifdef __ELIMINATION__
                   ,*(ps.trees)
                   #endif
                   );

   pthread_mutex_lock(&__idleMutex);
   __busyNum--;
   __idleNum++;
   pthread_mutex_unlock(&__idleMutex);

   //printf("2 %d %d\n",__busyNum,__taskQue.size());
   while(__busyNum||__taskQue.size()) { // while there is some other busy thread share the workload
      bool gotTask = false;
      //printf("[new iteration %d %d %d]\n",__busyNum,__idleNum,(int)__taskQue.size());
      if(__taskQue.size()) {
         pthread_mutex_lock(&__queMutex);
         if(__taskQue.size()) {
            ps = __taskQue.front();
            __taskQue.pop();
            gotTask = true;
         }
         pthread_mutex_unlock(&__queMutex);
      }
      if(gotTask) {
         pthread_mutex_lock(&__idleMutex);
         __busyNum++;
         __idleNum--;
         pthread_mutex_unlock(&__idleMutex);
         ps.circuit->dfs(ps.size,ps.visited,ps.used,ps.current_tree,*(ps.result)
                         #ifdef __ELIMINATION__
                         ,*(ps.trees)
                         #endif
                         );
         pthread_mutex_lock(&__idleMutex);
         __busyNum--;
         __idleNum++;
         pthread_mutex_unlock(&__idleMutex);
      } else pthread_yield();
   }
   pthread_mutex_lock(&__idleMutex);
   __idleNum--;
   pthread_mutex_unlock(&__idleMutex);
   //printf("<finished>\n");
   return NULL;
}
void Circuit::enumParallel(
      int size,
      vector<bool>& visited,
      vector<const Element*>& current_tree,
      vector<vector<const Element*> >& result
#ifdef __ELIMINATION__
      ,vector<pair<char , unsigned long long> >& trees
#endif // __ELIMINATION__
      ) {

   /* initialize task queue */
   while(!__taskQue.empty())
      __taskQue.pop();
#ifdef __ELIMINATION__
   __taskQue.push(PrimState(size, visited, current_tree, &result, &trees, this));
#else
   __taskQue.push(PrimState(size, visited, current_tree, &result, this));
#endif
   pthread_mutex_init(&__queMutex, NULL);
   pthread_mutex_init(&__treeMutex, NULL);
   pthread_mutex_init(&__idleMutex, NULL);

   /* start parallel threads */
   // process tasks in "phase"
   pthread_t* thHandles = (pthread_t*)malloc(threadcnt*sizeof(pthread_t));
   PrimState* states = new PrimState[threadcnt];

   while(__taskQue.size()) {
      int startnum = ((int)__taskQue.size()<threadcnt? (int)__taskQue.size():threadcnt);
      //printf("<%d %d>\n",__taskQue.size(),startnum);
      // start how many thread?
      for(int i=0; i<startnum; i++) {
         states[i]=__taskQue.front();
         __taskQue.pop();
      }
      for(int i=0; i<startnum; i++)
         pthread_create(thHandles+i, NULL, processTask, (void*)(states+i));
      for(int i=0; i<startnum; i++)
         pthread_join(thHandles[i], NULL);
      if((int)__taskQue.size()>=threadcnt) break;
   }

   /* parallel DFS */
//   int startnum = __taskQue.size();
   int startnum = threadcnt<(int)__taskQue.size()? threadcnt:(int)__taskQue.size();
   __busyNum = 0;
   __idleNum = startnum;
//   printf("<threadcnt: %d, startnum: %d>\n",threadcnt,startnum);
   delete [] states;
   free(thHandles);
   states = new PrimState[startnum];
   thHandles = (pthread_t*)malloc(startnum*sizeof(pthread_t));
   for(int i=0; i<startnum; i++) {
      states[i]=__taskQue.front();
      __taskQue.pop();
   }
   for(int i=0; i<startnum; i++)
      pthread_create(thHandles+i, NULL, dfsAdapter, (void*)(states+i));
   for(int i=0; i<startnum; i++)
      pthread_join(thHandles[i], NULL);

   /* free pthread handles */
   free(thHandles);
   delete [] states;
   pthread_mutex_destroy(&__queMutex);
   pthread_mutex_destroy(&__treeMutex);
   pthread_mutex_destroy(&__idleMutex);

}

#endif // __PARALLEL__

void Circuit::dfs(
      int size,
      vector<bool>& visited,
      vector<vector<bool> >& used,
      vector<const Element*>& current_tree,
      vector<vector<const Element*> >& result
#ifdef __ELIMINATION__
      ,vector<pair<char , unsigned long long> >& trees
#endif
      ) {//printf("[?]\n");
   // visited[i]  -> has nodes[i] been contained in the tree yet?
   // used[i][j]  -> has nodes[i].connections[j] been used yet?
   // current_tree-> an array storing current tree edges
   // result      -> an array storing tree edges of all spanning trees

   // check if all the nodes are used
   bool done = true ;

#ifdef __PARALLEL__
   // flag to avoid forking the first task
   bool forkflag = false;
#endif

   vector<pair<int , int> > recoverList ;

   for(int v = 0 ; v < size ; ++ v) {
      if(visited[v] == false) {
         done = false ;
         int amountOfConnections = this->nodes[v]->connections.size() ;
         for(int i = 0 ; i < amountOfConnections ; ++ i) {
            unsigned desId = this->nodes[v]->connections[i].destination->nodeId ;
            unsigned desIndex = getIndexById(desId) ;

            if(visited[desIndex] == true && used[v][i] == false) {
               recoverList.push_back(pair<int , int>(v , i)) ;
               visited[v] = true ;
               propagateEquivalents(v , visited) ;
               used[v][i] = true ;
               current_tree.push_back(this->nodes[v]->connections[i].element) ;

/**************************** distribute tasks if necessary ******************************/
#ifdef __PARALLEL__
bool didPush = false;
if(forkflag && __idleNum>(int)__taskQue.size()) {
   pthread_mutex_lock(&__idleMutex);
   pthread_mutex_lock(&__queMutex);
   // the lock resides inside the preliminary if condition to avoid frequent mutex access
   // the __idleNum however, must be judged again to avoid racing condition
   if(__idleNum>(int)__taskQue.size()) {
#ifdef __ELIMINATION__
   __taskQue.push(PrimState(size, visited, used, current_tree, &result, &trees, this));
#else
   __taskQue.push(PrimState(size, visited, used, current_tree, &result, this));
#endif
   didPush = true;
   }
   pthread_mutex_unlock(&__idleMutex);
   pthread_mutex_unlock(&__queMutex);
}
if(!didPush) {
#endif
/*****************************************************************************************/
#ifdef __ELIMINATION__
               this->dfs(size , visited , used , current_tree , result , trees) ;
#else
               this->dfs(size , visited , used , current_tree , result) ;
#endif

#ifdef __PARALLEL__
forkflag = true;
}
#endif
/*****************************************************************************************/
               current_tree.pop_back() ;
               visited[v] = false ;
               propagateEquivalents(v , visited) ;
            }
         }
      }
   }

   // recover used
   int recoverListLength = recoverList.size() ;
   for(int i = 0 ; i < recoverListLength ; ++ i) {
      used[recoverList[i].first][recoverList[i].second] = false ;
   }

   if(done) {
#ifdef __PARALLEL__
      pthread_mutex_lock(&__treeMutex);
#endif
#ifdef __ELIMINATION__
      // check if elimination could be done
      elimSub(current_tree,result,trees);
#else // no __ELIMINATION__
      result.push_back(current_tree) ;
#endif // __ELIMINATION__
#ifdef __PARALLEL__
      pthread_mutex_unlock(&__treeMutex);
#endif
   }
}

vector<vector<const Element*> > Circuit::enumTree(const Node * refNode) {
   vector<vector<const Element*> > result ;
   vector<const Element*> current_tree ;
   int size = this->nodes.size() ;
   unsigned refNodeIndex = getIndexById(refNode->nodeId) ;

   vector<bool> visited(size , false) ;
   vector<vector<bool> > used(size) ;
#ifdef __ELIMINATION__
   // pair of sign, sum of hashed element names
   vector<pair<char , unsigned long long> > trees ;
#endif // __ELIMINATION__
   for(int i = 0 ; i < size ; i ++) {
      int size_2 = this->nodes[i]->connections.size() ;
      used[i] = vector<bool>(size_2 , false) ;
   }

   visited[refNodeIndex] = true ;
   propagateEquivalents(refNodeIndex , visited) ;

#ifdef __PARALLEL__

#ifdef __ELIMINATION__
   this->enumParallel(size, visited , current_tree , result , trees) ;
#else
   this->enumParallel(size, visited , current_tree , result) ;
#endif

#else

#ifdef __ELIMINATION__
   this->dfs(size, visited , used , current_tree , result , trees) ;
#else
   this->dfs(size, visited , used , current_tree , result) ;
#endif

#endif // __PARALLEL__

   return result ;
}

unsigned Circuit::getIndexById(unsigned id) {
   map<unsigned, unsigned>::const_iterator it = idMap.find(id) ;
   if(it == idMap.end()) {
      // node not exist
      unsigned index = nodes.size() ;
      nodes.push_back(new Node(id)) ;
      idMap.insert(pair<unsigned , unsigned>(id , index)) ;
      return index ;
   } else {
      return (*it).second ;
   }
}

Node * Circuit::getNodeById(unsigned id) { // consturct new node if not exist
   return nodes[getIndexById(id)] ;
}

void Node::setConnect(const Node * dest, const Element* element) {
   this->connections.push_back(Connection(dest , element)) ;
}

void Node::addEquivalent(const Node * node , const VoltageSource * reason) {
   this->equivalents.push_back(Equivalent(node , reason)) ;
}

Circuit::~Circuit() {
   int size = nodes.size() ;
   for(int i = 0 ; i < size ; ++ i) {
      delete nodes[i] ;
   }
   size = elements.size() ;
   for(int i = 0 ; i < size ; ++ i) {
      delete elements[i] ;
   }
   size = sources.size() ;
   for(int i = 0 ; i < size ; ++ i) {
      delete sources[i] ;
   }
}

void Circuit::print() const {
   unsigned size = nodes.size() ;
   cout << "===== Circuit Detail =====" << endl;
   for(unsigned i = 0; i < size ; ++ i) {
      cout << "[" << setw(2) << nodes[i]->nodeId << "]" << endl;
      for(unsigned j = 0; j < nodes[i]->connections.size(); ++j) {
         cout << " -> [" << setw(2) << nodes[i]->connections[j].destination->nodeId << "] " << *(nodes[i]->connections[j].element) << endl ;
      }
   }
   cout << "--------- Source ---------" << endl ;
   for(vector<Source *>::const_iterator it = sources.begin() ;
         it != sources.end() ; ++ it) {
      cout << (*it)->name() << " [" << (*it)->node1() << "] -> [" << (*it)->node2() <<
         "] " << (*it)->prevValue() << " " << (*it)->nextValue() << endl ;
   }
   cout << "==========================" << endl;
}

