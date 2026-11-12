/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <EMotionFX/Source/Actor.h>
#include <EMotionFX/Source/Node.h>
#include <EMotionFX/Source/Skeleton.h>
#include <Tests/SystemComponentFixture.h>

namespace EMotionFX
{
    TEST_F(SystemComponentFixture, MatchNodeMotionSourcesByNamesPairsLeftAndRightMarkers)
    {
        Actor* actor = aznew Actor("TestActor");
        Skeleton* skeleton = actor->GetSkeleton();
        actor->AddNode(Node::Create("root", skeleton));
        actor->AddNode(Node::Create("hand_L", skeleton));
        actor->AddNode(Node::Create("hand_R", skeleton));
        actor->AddNode(Node::Create("Bip01 L Forearm", skeleton));
        actor->AddNode(Node::Create("Bip01 R Forearm", skeleton));
        actor->AddNode(Node::Create("Left Hand", skeleton));
        actor->AddNode(Node::Create("Right Hand", skeleton));
        actor->AddNode(Node::Create("spine_01", skeleton));

        actor->AllocateNodeMirrorInfos();
        actor->MatchNodeMotionSourcesByNames();

        EXPECT_EQ(actor->GetNodeMirrorInfo(1).m_sourceNode, 2) << "hand_L should be paired with hand_R.";
        EXPECT_EQ(actor->GetNodeMirrorInfo(2).m_sourceNode, 1) << "hand_R should be paired with hand_L.";
        EXPECT_EQ(actor->GetNodeMirrorInfo(3).m_sourceNode, 4) << "A standalone L marker should pair the forearms.";
        EXPECT_EQ(actor->GetNodeMirrorInfo(4).m_sourceNode, 3);
        EXPECT_EQ(actor->GetNodeMirrorInfo(5).m_sourceNode, 6) << "A Left/Right word should pair the hands.";
        EXPECT_EQ(actor->GetNodeMirrorInfo(6).m_sourceNode, 5);
        EXPECT_EQ(actor->GetNodeMirrorInfo(7).m_sourceNode, 7)
            << "A node without a left/right marker keeps mirroring itself.";

        delete actor;
    }
} // end namespace EMotionFX
