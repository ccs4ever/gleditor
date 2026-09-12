% ==============================================================================
% family.pl - Multi-Generational Family Tree and Lineage Knowledge Base
% ==============================================================================

% ------------------------------------------------------------------------------
% 1. Gender Classifications
% ------------------------------------------------------------------------------

male(prince_albert).
male(edward_vii).
male(george_v).
male(george_vi).
male(edward_viii).
male(henry_gloucester).
male(george_kent).
male(prince_philip).
male(charles_iii).
male(andrew_duke).
male(edward_edinburgh).
male(william_wales).
male(harry_sussex).
male(george_wales).
male(louis_wales).
male(archie_sussex).

female(queen_victoria).
female(queen_alexandra).
female(queen_mary).
female(queen_elizabeth_queen_mother).
female(mary_princess).
female(elizabeth_ii).
female(margaret).
female(diana_spencer).
female(anne_princess).
female(catherine_middleton).
female(meghan_markle).
female(charlotte_wales).
female(lilibet_sussex).

% ------------------------------------------------------------------------------
% 2. Parentage Relations: parent(Parent, Child)
% ------------------------------------------------------------------------------

% Generation -1 -> 0 (Victoria & Albert -> Edward VII)
parent(queen_victoria, edward_vii).
parent(prince_albert, edward_vii).

% Generation 0 -> 1 (Edward VII & Alexandra -> George V)
parent(edward_vii, george_v).
parent(queen_alexandra, george_v).

% Generation 1 -> 2
parent(george_v, george_vi).
parent(queen_mary, george_vi).
parent(george_v, edward_viii).
parent(queen_mary, edward_viii).
parent(george_v, mary_princess).
parent(queen_mary, mary_princess).
parent(george_v, henry_gloucester).
parent(queen_mary, henry_gloucester).
parent(george_v, george_kent).
parent(queen_mary, george_kent).

% Generation 2 -> 3
parent(george_vi, elizabeth_ii).
parent(queen_elizabeth_queen_mother, elizabeth_ii).
parent(george_vi, margaret).
parent(queen_elizabeth_queen_mother, margaret).

% Generation 3 -> 4
parent(elizabeth_ii, charles_iii).
parent(prince_philip, charles_iii).
parent(elizabeth_ii, anne_princess).
parent(prince_philip, anne_princess).
parent(elizabeth_ii, andrew_duke).
parent(prince_philip, andrew_duke).
parent(elizabeth_ii, edward_edinburgh).
parent(prince_philip, edward_edinburgh).

% Generation 4 -> 5
parent(charles_iii, william_wales).
parent(diana_spencer, william_wales).
parent(charles_iii, harry_sussex).
parent(diana_spencer, harry_sussex).

% Generation 5 -> 6
parent(william_wales, george_wales).
parent(catherine_middleton, george_wales).
parent(william_wales, charlotte_wales).
parent(catherine_middleton, charlotte_wales).
parent(william_wales, louis_wales).
parent(catherine_middleton, louis_wales).

parent(harry_sussex, archie_sussex).
parent(meghan_markle, archie_sussex).
parent(harry_sussex, lilibet_sussex).
parent(meghan_markle, lilibet_sussex).

% ------------------------------------------------------------------------------
% 3. Rules & Lineage Axioms
% ------------------------------------------------------------------------------

father(F, C) :- parent(F, C), male(F).
mother(M, C) :- parent(M, C), female(M).

grandparent(GP, GC) :- parent(GP, P), parent(P, GC).
great_grandparent(GGP, GGC) :- parent(GGP, GP), grandparent(GP, GGC).

ancestor(A, D) :- parent(A, D).
ancestor(A, D) :- parent(A, Z), ancestor(Z, D).

descendant(D, A) :- ancestor(A, D).

sibling(X, Y) :- parent(P, X), parent(P, Y), X \= Y.
