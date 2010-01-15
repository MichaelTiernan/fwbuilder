/* 

                          Firewall Builder

                 Copyright (C) 2007 NetCitadel, LLC

  Author:  Vadim Kurland     vadim@vk.crocodile.org

  $Id$

  This program is free software which we release under the GNU General Public
  License. You may redistribute and/or modify this program under the terms
  of that license as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
 
  To get a copy of the GNU General Public License, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include "config.h"

#include "PolicyCompiler_iosacl.h"

#include "fwbuilder/FWObjectDatabase.h"
#include "fwbuilder/RuleElement.h"
#include "fwbuilder/IPService.h"
#include "fwbuilder/ICMPService.h"
#include "fwbuilder/TCPService.h"
#include "fwbuilder/UDPService.h"
#include "fwbuilder/Network.h"
#include "fwbuilder/Policy.h"
#include "fwbuilder/Interface.h"
#include "fwbuilder/Management.h"
#include "fwbuilder/Resources.h"
#include "fwbuilder/AddressTable.h"

#include <iostream>
#if __GNUC__ > 3 || \
    (__GNUC__ == 3 && (__GNUC_MINOR__ > 2 || (__GNUC_MINOR__ == 2 ) ) ) || \
     _MSC_VER
#  include <streambuf>
#else
#  include <streambuf.h>
#endif
#include <iomanip>
#include <algorithm>
#include <functional>

#include <assert.h>

using namespace libfwbuilder;
using namespace fwcompiler;
using namespace std;

string PolicyCompiler_iosacl::myPlatformName() { return "iosacl"; }

PolicyCompiler_iosacl::PolicyCompiler_iosacl(FWObjectDatabase *_db,
                                             Firewall *fw,
                                             bool ipv6_policy,
                                             OSConfigurator *_oscnf) :
    PolicyCompiler_cisco(_db, fw, ipv6_policy, _oscnf)
{
    resetinbound=false;
    fragguard=false;
}

int PolicyCompiler_iosacl::prolog()
{
    string version = fw->getStr("version");
    string platform = fw->getStr("platform");
    string host_os = fw->getStr("host_OS");

    if (platform!="iosacl")
	abort("Unsupported platform " + platform );

    object_groups = new Group();
    dbcopy->add( object_groups );

//    output << "!################"  << endl;

    return PolicyCompiler::prolog();
}

void PolicyCompiler_iosacl::addDefaultPolicyRule()
{
    PolicyCompiler_cisco::addDefaultPolicyRule();

/*
 * PolicyCompiler_cisco::addDefaultPolicyRule() adds a rule to permit
 * backup ssh access to the firewall. Since IOS ACL are stateless, we
 * need to add another rule to permit reply packets.
 */
    if ( getCachedFwOpt()->getBool("mgmt_ssh") &&
         !getCachedFwOpt()->getStr("mgmt_addr").empty() )
    {
        TCPService *ssh_rev = dbcopy->createTCPService();
        ssh_rev->setSrcRangeStart(22);
        ssh_rev->setSrcRangeEnd(22);
        dbcopy->add(ssh_rev, false);

        Network *mgmt_workstation = dbcopy->createNetwork();
        mgmt_workstation->setAddressNetmask(
            getCachedFwOpt()->getStr("mgmt_addr"));

        dbcopy->add(mgmt_workstation, false);

        PolicyCompiler::addMgmtRule(
            fw, mgmt_workstation, ssh_rev,
            NULL, PolicyRule::Outbound, PolicyRule::Accept,
            "backup ssh access rule (out)");
    }
}

bool PolicyCompiler_iosacl::checkForDynamicInterface::findDynamicInterface(
    PolicyRule *rule, RuleElement *rel)
{
    string vers=compiler->fw->getStr("version");
    for (list<FWObject*>::iterator i1=rel->begin(); i1!=rel->end(); ++i1) 
    {
	FWObject *o   = *i1;
	FWObject *obj = NULL;
	if (FWReference::cast(o)!=NULL) obj=FWReference::cast(o)->getPointer();
        Interface *iface=Interface::cast(obj);
        if (iface!=NULL && iface->isDyn())
            compiler->abort(
                rule, 
                "Dynamic interface can not be used in the IOS ACL rules.");
    }

    return true;
}

bool PolicyCompiler_iosacl::checkForDynamicInterface::processNext()
{
    PolicyRule     *rule=getNext(); if (rule==NULL) return false;

    findDynamicInterface(rule,rule->getSrc());
    findDynamicInterface(rule,rule->getDst());

    tmp_queue.push_back(rule);
    return true;
}

bool PolicyCompiler_iosacl::SpecialServices::processNext()
{
    PolicyCompiler_iosacl *iosacl_comp=dynamic_cast<PolicyCompiler_iosacl*>(compiler);
    PolicyRule *rule=getNext(); if (rule==NULL) return false;
    Service    *s=compiler->getFirstSrv(rule);

    if (IPService::cast(s)!=NULL)
    {
	if (s->getBool("rr")        ||
	    s->getBool("ssrr")      ||
	    s->getBool("ts") )
	    compiler->abort(
                    rule, 
                    "IOS ACL does not support checking for IP options in ACLs.");
    }
    if (TCPService::cast(s)!=NULL && TCPService::cast(s)->inspectFlags())
    {
        string version = compiler->fw->getStr("version");
        if (XMLTools::version_compare(version, "12.4")<0)
            compiler->abort(rule, "TCP flags match requires IOS v12.4 or later.");
    }

    tmp_queue.push_back(rule);
    return true;
}

void PolicyCompiler_iosacl::compile()
{
    string banner = " Compiling ruleset " + getSourceRuleSet()->getName();
    if (ipv6) banner += ", IPv6";
    info(banner);

    try 
    {
        string vers = fw->getStr("version");
        string platform = fw->getStr("platform");

	Compiler::compile();

	addDefaultPolicyRule();

        if ( fw->getOptionsObject()->getBool ("check_shading") &&
             ! inSingleRuleCompileMode())
        {
            add( new Begin("Detecting rule shadowing"               ) );
            add( new printTotalNumberOfRules());

            add( new ItfNegation("process negation in Itf"  ) );
            add( new InterfacePolicyRules(
                     "process interface policy rules and store interface ids"));

            add( new recursiveGroupsInSrc("check for recursive groups in SRC"));
            add( new recursiveGroupsInDst("check for recursive groups in DST"));
            add( new recursiveGroupsInSrv("check for recursive groups in SRV"));

            add( new ExpandGroups("expand groups"));
            add( new dropRuleWithEmptyRE(
                     "drop rules with empty rule elements"));
            add( new eliminateDuplicatesInSRC("eliminate duplicates in SRC"));
            add( new eliminateDuplicatesInDST("eliminate duplicates in DST"));
            add( new eliminateDuplicatesInSRV("eliminate duplicates in SRV"));
            add( new ExpandMultipleAddressesInSrc(
                     "expand objects with multiple addresses in SRC" ) );
            add( new ExpandMultipleAddressesInDst(
                     "expand objects with multiple addresses in DST" ) );
            add( new dropRuleWithEmptyRE(
                     "drop rules with empty rule elements"));
            add( new ConvertToAtomic("convert to atomic rules"       ) );
            add( new DetectShadowing("Detect shadowing"              ) );
            add( new simplePrintProgress() );

            runRuleProcessors();
            deleteRuleProcessors();
        }


        add( new Begin (" Start processing rules" ) );
        add( new printTotalNumberOfRules ( ) );

        add( new singleRuleFilter());

        add( new recursiveGroupsInSrc( "check for recursive groups in SRC" ) );
        add( new recursiveGroupsInDst( "check for recursive groups in DST" ) );
        add( new recursiveGroupsInSrv( "check for recursive groups in SRV" ) );

        add( new emptyGroupsInSrc( "check for empty groups in SRC" ) );
        add( new emptyGroupsInDst( "check for empty groups in DST" ) );
        add( new emptyGroupsInSrv( "check for empty groups in SRV" ) );

        add( new ExpandGroups ("expand groups" ) );
        add( new dropRuleWithEmptyRE(
                 "drop rules with empty rule elements"));
        add( new eliminateDuplicatesInSRC( "eliminate duplicates in SRC" ) );
        add( new eliminateDuplicatesInDST( "eliminate duplicates in DST" ) );
        add( new eliminateDuplicatesInSRV( "eliminate duplicates in SRV" ) );

        add( new processMultiAddressObjectsInSrc(
                 "process MultiAddress objects in Src") );
        add( new processMultiAddressObjectsInDst(
                 "process MultiAddress objects in Dst") );

        add( new ItfNegation( "process negation in Itf" ) );
        add( new InterfacePolicyRules(
                 "process interface policy rules and store interface ids") );

        add( new splitServices ("split rules with different protocols" ) );

        add( new ExpandMultipleAddressesInSrc(
                 "expand objects with multiple addresses in SRC" ) );
        add( new MACFiltering ("check for MAC address filtering" ) );
//        add( new splitByNetworkZonesForSrc ("split rule if objects in Src belong to different network zones " ) );
//        add( new replaceFWinDSTPolicy ("replace fw with its interface in DST in global policy rules") );

        add( new ExpandMultipleAddressesInDst(
                 "expand objects with multiple addresses in DST" ) );
        add( new MACFiltering(
                 "check for MAC address filtering" ) );
        add( new dropRuleWithEmptyRE("drop rules with empty rule elements"));

//        add( new splitByNetworkZonesForDst ("split rule if objects in Dst belong to different network zones " ) );

        if (ipv6)
            add( new DropIPv4Rules("drop ipv4 rules"));
        else
            add( new DropIPv6Rules("drop ipv6 rules"));
        add( new dropRuleWithEmptyRE("drop rules with empty rule elements"));

        add( new checkForUnnumbered("check for unnumbered interfaces"));

        add( new addressRanges ("process address ranges" ) );
        add( new dropRuleWithEmptyRE("drop rules with empty rule elements"));

        add( new setInterfaceAndDirectionBySrc(
    "Set interface and direction for rules with interface 'all' using SRC"));
        add( new setInterfaceAndDirectionByDst(
    "Set interface and direction for rules with interface 'all' using DST"));
        add( new setInterfaceAndDirectionIfInterfaceSet(
                 "Set direction for rules with interface not 'all'"));

        add( new specialCaseWithDynInterface(
                 "check for a special cases with dynamic interface" ) );

        // first arg is true because we use "ip access-list" for IOS.
        add( new pickACL( true, "assign ACLs" ) );

        add( new SpecialServices( "check for special services" ) );
        add( new CheckForUnsupportedUserService("check for user service") );

        add( new checkForZeroAddr(    "check for zero addresses" ) );
        add( new checkForDynamicInterface("check for dynamic interfaces" ) );

        /* remove redundant objects only after all splits has been
         * done, right before object groups are created
         */
        add( new removeRedundantAddressesFromSrc(
                 "remove redundant addresses from Src") );
        add( new removeRedundantAddressesFromDst(
                 "remove redundant addresses from Dst") );

        add( new ConvertToAtomic ("convert to atomic rules" ) );

        add( new simplePrintProgress());

        add( new createNewCompilerPass ("Creating ACLs ..."));

//        add( new ClearACLs("Clear ACLs"));

        // This processor prints each ACL separately in one block.
        // It adds comments inside to denote original rules.
        //
        add( new PrintCompleteACLs("Print ACLs"));

//        add( new PrintRule("generate code for ACLs"));
        add( new simplePrintProgress());

        runRuleProcessors();

    } catch (FWException &ex)
    {
	error(ex.toString());
        exit(1);
    }
}

string PolicyCompiler_iosacl::printAccessGroupCmd(ciscoACL *acl)
{
    ostringstream str;

    string addr_family_prefix = "ip";
    if (ipv6) addr_family_prefix = "ipv6";

    if (getSourceRuleSet()->isTop())
    {
        string dir;
        if (acl->direction()=="in"  || acl->direction()=="Inbound")  dir="in";
        if (acl->direction()=="out" || acl->direction()=="Outbound") dir="out";

        str << "interface " << acl->getInterface()->getName() << endl;
        str << "  " << addr_family_prefix << " ";
        str << getAccessGroupCommandForAddressFamily(ipv6);
        str << " " << acl->workName() << " " << dir << endl;
        str << "exit" << endl;
    }
    return str.str();
}

void PolicyCompiler_iosacl::epilog()
{
    output << endl;

    for (map<string,ciscoACL*>::iterator i=acls.begin(); i!=acls.end(); ++i) 
    {
        ciscoACL *acl=(*i).second;
        if (acl->size()!=0) output << printAccessGroupCmd(acl);
    }
    output << endl;

    if ( fw->getOptionsObject()->getBool("iosacl_regroup_commands") ) 
    {
        info(" Regrouping commands");
        regroup();
    }
}

string PolicyCompiler_iosacl::getAccessGroupCommandForAddressFamily(bool ipv6)
{
    if (ipv6) return "traffic-filter";
    return "access-group";
}
