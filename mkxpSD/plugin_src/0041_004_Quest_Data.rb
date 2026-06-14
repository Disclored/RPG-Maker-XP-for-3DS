module QuestModule
  
  # You don't actually need to add any information, but the respective fields in the UI will be blank or "???"
  # I included this here mostly as an example of what not to do, but also to show it's a thing that exists
  Quest0 = {
  
  }
  
  # Here's the simplest example of a single-stage quest with everything specified
  Quest1 = {
    :ID => "1",
    :Name => "The Battle for the Crown",
    :QuestGiver => "You",
    :Location0 => "Gardenia Academy",
    :Location1 => :Location0,
    :QuestDescription => "Your life-long dream is within grasp now that you've collected the 8 Gym Badges. Head to Gardenia Academy, participate in and win the Solaria Coronation Tournament, and be crowned the new Solar Monarch.",
    :RewardString => "Being crowned Solar Monarch"
  }
  
  # Here's an extension of the above that includes multiple stages
  Quest2 = {
    :ID => "2",
    :Name => "The Successors",
    :QuestGiver => "Move Sage",
    :Location0 => "Holtlant Town",
    :Location1 => :Location0,
    :QuestDescription => "The Move Sage wants to pass down his teachings to 3 disciples. If you find anyone stuck in a rut who has potential, direct them to him.",
    :RewardString => "Move Tutoring"
  }
  
  # Here's an example of a quest with lots of stages that also doesn't have a stage location defined for every stage
  Quest3 = {
    :ID => "3",
    :Name => "The Fairy Queen of the Moon",
    :QuestGiver => "Diana",
    :Location0 => "Myrfield Gym",
    :Location1 => :Location0,
    :QuestDescription => "The new Myrfield Gym Leader Diana has asked you to be her first challenger. Defeat her in battle to earn your 10th Gym Badge.",
    :RewardString => "Gym Badge"
  }
  
  # Here's an example of not defining the quest giver and reward text
  Quest4 = {
    :ID => "4",
    :Name => "The Lost Pokénect",
    :QuestGiver => "Lost Pokénect Owner",
    :Location0 => "???",
    :Location1 => :Location0,
    :QuestDescription => "You found a Pokénect on the ground and were called by its owner, who is temporarily unable to pick it up. Hold on to the device until you get the call to meet up with the owner and return it to them.",
    :RewardString => "Pokénect Owner's Contact Details"
  }
  
  # Other random examples you can look at if you want to fill out the UI and check out the page scrolling
  Quest5 = {
    :ID => "5",
    :Name => "The Flute of the Stars",
    :QuestGiver => "Sol Inc. Researcher",
    :Location0 => "Sol Research Lab",
    :Location1 => :Location0,
    :QuestDescription => "A scientist in the Sol Research Lab has shared with you information on a flute that could summon a Pokémon of the cosmos. To restore the flute to its full power, he's asked you to show him the Elite Four for research.",
    :RewardString => "Nebula Flute"
  }
  
  Quest6 = {
    :ID => "6",
    :Name => "The Friendless Cosplay Girl",
    :QuestGiver => "Parasol Lady",
    :Location0 => "Jyeshtha City",
    :Location1 => :Location0,
    :QuestDescription => "A lady has informed you of a Cosplay Girl in Jyeshtha City who lacks friends, but is fond of Swadloon. Perhaps giving her a Swadloon will brighten up her life.",
    :RewardString => "nil"
  }
  
  Quest7 = {
    :ID => "7",
    :Name => "The Gym Challenge",
    :QuestGiver => "You",
    :Location0 => "Solaria",
    :Location1 => :Location0,
    :QuestDescription => "It is a great honour to participate in the Gym Challenge and you have been chosen to do so. Travel the Solaria region and challenge the 8 Gym Leaders to earn their Gym Badge.",
    :RewardString => "Privilege of participating in the SCT"
  }
  
  Quest8 = {
    :ID => "8",
    :Name => "The Delivery",
    :QuestGiver => "Amanda",
    :Location0 => "Jadevik City",
    :Location1 => :Location0,
    :QuestDescription => "Out of convenience, Amanda has asked you to deliver a package to Bruno in Jadevik City. You'll need to cross through Route 4, Telgior Mines and Route 5."
  }
  
  Quest9 = {
    :ID => "9",
    :Name => "The Nectar Dancer",
    :QuestGiver => "Junkarian Florist",
    :Location0 => "Junkar City",
    :Location1 => :Location0,
    :QuestDescription => "There's a Pokémon called Oricorio that can make use of the special flower nectar grown by a florist in Junkar City. If you show her an Oricorio, she'd be willing to sell you the nectar."
  }
  
  Quest10 = {
    :ID => "10",
    :Name => "The Power of Song and Dance",
    :QuestGiver => "Guitar Maker",
    :Location0 => "Jadevik City",
    :Location1 => :Location0,
    :QuestDescription => "The Spiky-Ear Pichu's ukulele has been repaired, but it seems like that is only the first step to help find its friend. Perhaps it could call forth its friend with the help of dancers."
  }
  
  Quest11 = {
    :ID => "11",
    :Name => "The Feelings of Gratitude",
    :QuestGiver => "Giltbert",
    :Location0 => "Gardenia Town",
    :Location1 => :Location0,
    :QuestDescription => "Giltbert has asked you to find him the Pokémon of Gratitude, supposedly found in flower fields. If you show it to him, he'll share with you a reward."
  }
  
  Quest12 = {
    :ID => "12",
    :Name => "The Strange Meteor",
    :QuestGiver => "Old Lady",
    :Location0 => "Meteor Hills",
    :Location1 => :Location0,
    :QuestDescription => "The old lady near the meteor in Junkar City has asked you to investigate Meteor Hills, following a tremor. If you find anything noteworthy there, be sure to show it to her for a reward."
  }
  
  Quest13 = {
    :ID => "13",
    :Name => "The Barefisted Spunky Punk!",
    :QuestGiver => "Emilia",
    :Location0 => "Zimbani Gym",
    :Location1 => :Location0,
    :QuestDescription => "Having taken care of all her administration work, Emilia is ready to hand you her Gym Badge... after a rematch.",
    :RewardString => "Gym Badge"
  }
  
  Quest14 = {
    :ID => "14",
    :Name => "The Lonely Lighthouse Beacon",
    :QuestGiver => "Rosy's Caretaker",
    :Location0 => "Jadevik Lighthouse",
    :Location1 => :Location0,
    :QuestDescription => "The caretaker of Rosy, the Jadevik Lighthouse's source of light, has asked you to provide her with some company. Show Rosy a Mareep, Flaaffy, or Ampharos to brighten her day."
  }
  
  Quest15 = {
    :ID => "15",
    :Name => "The Love Confession",
    :QuestGiver => "Lover Boy",
    :Location0 => "Zimbani City",
    :Location1 => :Location0,
    :QuestDescription => "A man in Zimbani City has asked for your help with proposing to his girlfriend. He requests that you give him a Spinda with a heart pattern. He will trade you the Spinda for a reward."
  }
  
  Quest16 = {
    :ID => "16",
    :Name => "The Hypnotist",
    :QuestGiver => "Sleepless Black Belt",
    :Location0 => "Towngor City",
    :Location1 => :Location0,
    :QuestDescription => "A restless man in Towngor City has asked you to show him a Pokémon knowing the move Hypnosis to help him fall asleep. If you present him with one, he'll give you a reward."
  }
  
  Quest17 = {
    :ID => "17",
    :Name => "The Shroom Maniac",
    :QuestGiver => "Poké Maniac",
    :Location0 => "Helianthus Poké Fan Club",
    :Location1 => :Location0,
    :QuestDescription => "An enthusiastic Pokémon fan from the Helianthus Poké Fan Club proclaimed his love for Shroomish to you. Perhaps something will happen if you show him one."
  }
  
  Quest18 = {
    :ID => "18",
    :Name => "The Trick to Victory",
    :QuestGiver => "Super Nerd",
    :Location0 => "R2-Yoko Gate",
    :Location1 => :Location0,
    :QuestDescription => "A Super Nerd has asked you to catch and show him a Yanma in Yoko Forest. He says he'll give you something useful for your journey if you do."
  }
  
  Quest19 = {
    :ID => "19",
    :Name => "The Dare",
    :QuestGiver => "Roughneck",
    :Location0 => "Towngor Tunnels",
    :Location1 => :Location0,
    :QuestDescription => "While travelling through the Towngor Tunnels, you got taunted by a Roughneck that your Pokémon aren't high enough level. Show him a high-level Pokémon to put him in his place."
  }
  
  Quest20 = {
    :ID => "20",
    :Name => "The Final Request",
    :QuestGiver => "Old man",
    :Location0 => "Dragon Tomb",
    :Location1 => :Location0,
    :QuestDescription => "Having lost his partner, an old man wishes to spend his final years not in misery but in joy. He has asked you to give him a Pokémon that he can raise, a Pokémon that's still weak."
  }
  
  Quest21 = {
    :ID => "21",
    :Name => "The Worthwhile Experience",
    :QuestGiver => "Scientist",
    :Location0 => "Holtlant Town",
    :Location1 => :Location0,
    :QuestDescription => "A scientist in Holtlant Town has told you about Exp. growth rates and how some Pokémon are slower to level up than others. If you show him such a Pokémon, he'll give you something worthwhile."
  }
  
  Quest22 = {
    :ID => "22",
    :Name => "The Fairly Enthusiastic Fairy Lover",
    :QuestGiver => "Fairy Tale Girl",
    :Location0 => "Zimbani City",
    :Location1 => :Location0,
    :QuestDescription => "A fairy-loving girl in Zimbani City has asked you to show her a Fairy-type Pokémon who shares a close bond with you. Despite her oddities, doing so might prove worthwhile."
  }
  
  Quest23 = {
    :ID => "23",
    :Name => "The Sky Battle",
    :QuestGiver => "Bird Keeper",
    :Location0 => "Route 7",
    :Location1 => :Location0,
    :QuestDescription => "Someone claiming to be a Sky Trainer has challenged you to a Sky Battle. You're only allowed to bring Flying-type Pokémon or Pokémon that are able to float. Knock this Sky Trainer down a peg for a reward."
  }
  
  Quest24 = {
    :ID => "24",
    :Name => "The Perfect Beverage",
    :QuestGiver => "Tea Maker",
    :Location0 => "Malum City",
    :Location1 => :Location0,
    :QuestDescription => "A tea maker in Malum City has asked you to help her create a new beverage. Show her a Chikorita, Fennekin, and Oshawott to help her create the beverage and she'll give you a reward."
  }
  
  Quest25 = {
    :ID => "25",
    :Name => "The Ghostly Theory",
    :QuestGiver => "Psychic",
    :Location0 => "R13-Holtlant Gate",
    :Location1 => :Location0,
    :QuestDescription => "A Psychic has asked you to give her a Pokémon with the ability Scrappy, an ability that lets it hit Ghost-Type Pokémon. She hopes that this could be used to fight back against the desert shadows."
  }
  
  Quest26 = {
    :ID => "26",
    :Name => "The Researcher",
    :QuestGiver => "School Kid",
    :Location0 => "R9-Zimbani Gate",
    :Location1 => :Location0,
    :QuestDescription => "A School Kid has told you about her favourite Pokémon: Minccino. Noticing that you have the Pokédex, she has asked you to show her the complete entry for Minccino."
  }
  
  Quest27 = {
    :ID => "27",
    :Name => "The Form Assistant Researcher",
    :QuestGiver => "Audrey",
    :Location0 => "Fitzberg Museum",
    :Location1 => :Location0,
    :QuestDescription => "A researcher, Audrey, has recently opened up a museum dedicated to Pokémon forms. To help her make accurate display statues, she has asked you to show her entries of different forms of the following Pokémon: Vivillon, Spinda, Minior, and Roselia. The forms only need to be seen, not necessarily caught."
  }
  
  Quest28 = {
    :ID => "28",
    :Name => "The Revival",
    :QuestGiver => "Scientist",
    :Location0 => "Junkar City",
    :Location1 => :Location0,
    :QuestDescription => "A scientist in Junkar City has told you about the fossils that can be found in the rocks of Telgior Mines, as well as of technology that can revive these fossils. Find an Armor Fossil with Rock Smash, revive it and then show it to the scientist for a reward."
  }

  Quest29 = {
    :ID => "29",
    :Name => "The Health Tip",
    :QuestGiver => "Nurse",
    :Location0 => "Farnlor City",
    :Location1 => :Location0,
    :QuestDescription => "A nurse in Farnlor City is asking you to show her a Pokémon that knows the move Heal Pulse, which might teach her some new skills. She's promised you a generous helping of healing items in return, which is something any Trainer would want!"
  }

 Quest30 = {
    :ID => "30",
    :Name => "The Toxic Treatment",
    :QuestGiver => "Pharmacist",
    :Location0 => "Myrfield City",
    :Location1 => :Location0,
    :QuestDescription => "A pharmacist in Myrfield City is particularly interested in Pokémon with the ability Poison Heal, which recover HP when poisoned. It looks like she'd have something good to give you if you give her one."
  }

 Quest31 = {
    :ID => "31",
    :Name => "The Cold Shoulder",
    :QuestGiver => "Poké Maniac",
    :Location0 => "Route 19",
    :Location1 => :Location0,
    :QuestDescription => "A Poké Maniac who is obsessed with the cold has asked you for a battle using only the coldest Pokémon. It's all chill, though - just find some Ice-type Pokémon who are up to the job. Maybe he'll find it in his cold heart to part with a reward should you defeat him."
  }

Quest32 = {
    :ID => "32",
    :Name => "The Fashion Disaster",
    :QuestGiver => "Beauty",
    :Location0 => "R18-Eisenblut Gate",
    :Location1 => :Location0,
    :QuestDescription => "A Beauty has a dilemma - nothing cute to wear on a night out with the girls. That is the ultimate crime. Find a beautiful Grass-type Pokémon that looks like it's rocking some killer footwear and show it to her. But beware: she has HIGH standards. What are you waiting for? Get searching!"
  }


Quest33 = {
    :ID => "33",
    :Name => "The Losing Streak",
    :QuestGiver => "Preschooler",
    :Location0 => "Route 6",
    :Location1 => :Location0,
    :QuestDescription => "A preschooler in the Route 6 preschool laments over her lack of success in battling. It's highly unusual for a skilled trainer like you, but... entertain this girl and let her have the sweet taste of victory!"
  }

Quest34 = {
    :ID => "34",
    :Name => "The Divine Intervention",
    :QuestGiver => "Hex Maniac",
    :Location0 => "Eisenblut City",
    :Location1 => :Location0,
    :QuestDescription => "A Hex Maniac in Eisenblut City who performs tarot card readings for a living would like to expand her knowledge, but she needs reference material. Show her a Sinistea, Clefairy, Delphox, and Honedge, and she will provide you with a handsome reward."
  }

Quest35 = {
    :ID => "35",
    :Name => "The Straw That Broke The...",
    :QuestGiver => "Lass",
    :Location0 => "R8-Malum Gate",
    :Location1 => :Location0,
    :QuestDescription => "After a disagreement, a Camerupt has stormed off towards Route 8 and left its trainer in quite a panic. Remedy this and reunite the pair! You've been promised compensation!"
  }

Quest36 = {
    :ID => "36",
    :Name => "The Trend",
    :QuestGiver => "Black Belt",
    :Location0 => "Farnlor Pokémon Center",
    :Location1 => :Location0,
    :QuestDescription => "A Black Belt is trying to concoct the latest in protein trends: protein water. With your help in collecting a Tamato, Figy, and Cheri Berry, he can bring his nourishing(?) creation to life!"
  }

Quest37 = {
    :ID => "37",
    :Name => "The Shining Request",
    :QuestGiver => "Aelia",
    :Location0 => "Sol Research Lab",
    :Location1 => :Location0,
    :QuestDescription => "Aelia is looking for a Pokémon that's level 100. She has mentioned something about a shiny Pokémon she will part with should you show her an especially powerful Pokémon.",
    :RewardString => "A shiny Pokémon"
  }

Quest38 = {
    :ID => "38",
    :Name => "The Tasty Request",
    :QuestGiver => "Lairus",
    :Location0 => "Sol Research Lab",
    :Location1 => :Location0,
    :QuestDescription => "Lairus is looking for a Lansat Berry which will take one of his dishes to the top. Show him one for something he personally cooked.",
    :RewardString => "His food"
  }

Quest39 = {
    :ID => "39",
    :Name => "The Handmade Request",
    :QuestGiver => "Sienna",
    :Location0 => "Sol Research Lab",
    :Location1 => :Location0,
    :QuestDescription => "Sienna is looking for a lost part she thinks might've ended up on Route 11. She'll give you one of her creations if you find it.",
    :RewardString => "Her creation"
  }

Quest40 = {
    :ID => "40",
    :Name => "The Dispel Magic",
    :QuestGiver => "Hex Maniac",
    :Location0 => "Jyeshtha City",
    :Location1 => :Location0,
    :QuestDescription => "A Hex Maniac shaman has told you about shadowy monsters that roam the desert at night. She has a charm that can dispel them, and is willing to give you the prototype version of it if you show her a Ghost Pokémon that is at least level 45."
  }

end
