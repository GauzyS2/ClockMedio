#include <LiquidCrystal.h>

//Pinos do LCD:
int rs = 19;
int e = 23;
int d4 = 4;
int d5 = 0;
int d6 = 2; 
int d7 = 15;

LiquidCrystal lcd(rs, e, d4, d5, d6, d7);

void setup() 
{
  lcd.begin(16, 2); //LCD de 16 colunas e 2 linhas.

  //Configura os caracteres personalizados:
  lcd.createChar(0, seta-esquerda); 
  lcd.createChar(1, seta_direita);
}

void loop()
{

}

//Criação dos caracteres personalizados:
byte seta-esquerda[8] = 
{
  B00000,
  B00010,
  B00110,
  B01110,
  B00110,
  B00010,
  B00000,
  B00000
};

byte seta-direita[8] = 
{
  B00000,
  B01000,
  B01100,
  B01110,
  B01100,
  B01000,
  B00000,
  B00000
};
