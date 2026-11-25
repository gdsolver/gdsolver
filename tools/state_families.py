"""state_families.py: the family of every member of struct State (restructure draft section 7.20, P9). A family is a group of fields one owner derives from the shadow together:
`shadow` = written from the shadow's body today; `other` = not yet assigned. tools/check_state_serial.py fails when a member has no family (so a new member must be placed). ASCII only."""

FAMILIES = {
    'input': 'gdHS gdHSb gdAux gdHS2 held action action2 pressSpent pressSpent2 jumpBuf jumpBuf2 latLock holdDead holdDead2 ringHold ringHold2 pcRel pcTouch edgeAge',
    'collision': 'snapObj snapDist snapObj2 snapDist2 groundUid onSlope onSlope2 slopeT slopeT2 slopeM slopeM2 slopeUid0 slopeUidNow slopeUid02 slopeUidNow2 rideLanded rideLanded2 seatT seatT2 seatSpd '
                 'ceilT ceilT2 ceilM4 ceilM42 ceilLatch ceilMode ceilPin stickFb stickSlope hitG hitG2 freeHalf',
    'ring_pad_orb': 'ogLinger usedOrb usedOrb2 usedPad usedPad2 usedOrbOld usedOrbOld2 usedOrbHist usedOrbHist2 touchRing touchRing2 touchRingT touchRingT2 portSeen portSeen2 a1cLatch rotBoost',
    'portal': 'spdFired spdFired2 spdMore spdMore2 spdSpill halfPrev halfPrev2 portalLatch portalLatch2 flipT flipT2 modeT modeT2 armT fgArm boost2 ball728',
    'band_dash': 'tight bandFloor bandCeil bandAnim bandBranch bandRefY pFlap pSpiderTap pNoTerm rHover rHover2 spiderJumpT slideT slideT2 dashNeg dashF dashAng dashSlope dashSlope2 lawSv prevC6q',
    'rotation_frame': 'rot rotPrev rotNeg rotNeg2 rotStep rotStep2 rotGm rotSpent rotChan rotRev rot2 frame frameChg revOff revCarry tpFromX tpMarkA tpMarkB tpArrive pExitVy tpSkip',
    'world_trigger': 'fireB coins taps tapsX items tog xgate evgate evKey tapN tapAge tapWin dxHist xcId xcLag xcOver followF followOk followOwn followN trig trigT',
    'shadow': 'y vy mode flip grounded xAbs dx dx2 rev dual mini boost ctrlT gravTouch tpBack tpTouch revFired lockOff dashing dashing2 y2 vy2 mode2 mini2 flip2 grounded2 rev2 slRef slRef2 prevWX prevWY',
    'lineage': 'parent',
}


def family_of():
    m = {}
    for fam, names in FAMILIES.items():
        for n in names.split():
            m[n] = fam
    return m